// Port of lambda/matching.ml (TYPECHECKER.md stage 10): compilation of
// pattern matching (Le Fessant-Maranget, "Optimizing Pattern-Matching",
// ICFP'2001), with the parts of typing/patterns.ml it uses (Patterns.Head,
// the views).  Switches go through the port of lambda/switch.ml.
//
// The pattern "views" of patterns.ml (General / Half_simple / Simple) are
// static refinements of Typedtree patterns; here they are plain Typedtree
// patterns and view / erase are the identity.  (erase drops a
// Tpat_construct's type annotation, which no function below reads.)
//
// OCaml's evaluation order is kept wherever it creates idents (stamps) or
// exit numbers: application and constructor arguments right to left,
// records in type-definition field order right to left, List.map / iter
// left to right, fold_right from the end, `let .. and ..` left to right.
//
// Not ported: the -dmatchcomp debug output (dbg / debugf and the pretty
// printers); the Degraded_to_partial_match warning is not emitted (warning
// messages are stage 9).
#include "cppcaml/typing/matching.hpp"

#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/ocaml_list.hpp"
#include "cppcaml/typing/parmatch.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/switch.hpp"
#include "cppcaml/typing/typeopt.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::matching {

namespace {

namespace tt = typedtree;
namespace L = lambda;
using lam = L::lambda;
using PK = tt::PatternDesc::Kind;
using namespace types;
using CK = tt::Constant::Kind;
using LK = L::LK;
using PrimK = L::Primitive::K;
using L::LetKind;
using tt::Partial;
using typedtree::as;

using clflags::afl_instrument;
using clflags::match_context_rows;
using clflags::safer_matching;

[[noreturn]] void fatal_error(const char* msg) { throw std::logic_error(msg); }

// ---- small Lambda helpers ------------------------------------------------------------
L::Primitive pfield(long n, L::ImmediateOrPointer ptr, MutableFlag mut) {
  L::Primitive p = L::prim(PrimK::Pfield);
  p.n = n;
  p.ptr = ptr;
  p.mut = mut;
  return p;
}
L::Primitive pfloatfield(long n) {
  L::Primitive p = L::prim(PrimK::Pfloatfield);
  p.n = n;
  return p;
}
L::Primitive pintcomp(L::IntegerComparison c) {
  L::Primitive p = L::prim(PrimK::Pintcomp);
  p.icmp = c;
  return p;
}
L::Primitive pfloatcomp(L::FloatComparison c) {
  L::Primitive p = L::prim(PrimK::Pfloatcomp);
  p.fcmp = c;
  return p;
}
L::Primitive pbintcomp(BoxedInteger bi, L::IntegerComparison c) {
  L::Primitive p = L::prim(PrimK::Pbintcomp);
  p.bi = bi;
  p.icmp = c;
  return p;
}
L::Primitive pccall(const PrimitiveDescription* d) {
  L::Primitive p = L::prim(PrimK::Pccall);
  p.ccall = d;
  return p;
}
L::Primitive praise(L::RaiseKind k) {
  L::Primitive p = L::prim(PrimK::Praise);
  p.raise = k;
  return p;
}
L::Primitive poffsetint(long n) {
  L::Primitive p = L::prim(PrimK::Poffsetint);
  p.n = n;
  return p;
}
L::Primitive parray(PrimK k, L::ArrayKind kind) {
  L::Primitive p = L::prim(k);
  p.array = kind;
  return p;
}
// Pmakeblock (tag, Immutable, None)
L::Primitive pmakeblock_immutable(long tag) {
  L::Primitive p = L::prim(PrimK::Pmakeblock);
  p.n = tag;
  p.mut = MutableFlag::Immutable;
  return p;
}
lam lprim(const L::Primitive& p, std::vector<lam> args, const L::ScopedLocation& loc) {
  return L::lprim(p, slice(args), loc);
}
lam lconst_int(long n) { return L::lconst(L::const_int(n)); }
lam lconst_immstring(std::string_view s) {
  auto* c = make<L::StructuredConstant>();
  c->kind = L::StructuredConstant::Kind::Const_immstring;
  c->s = s;
  return L::lconst(c);
}
lam lstaticraise(long i, std::vector<lam> args = {}) { return L::lstaticraise(i, slice(args)); }
L::ScopedLocation loc_unknown() { return {}; }

// Primitive.simple ~name ~arity ~alloc
const PrimitiveDescription* primitive_simple(std::string_view name, long arity, bool alloc) {
  ZoneScope perm(permanent_zone());
  auto* d = make<PrimitiveDescription>();
  d->prim_name = zstr(name);
  d->prim_arity = arity;
  d->prim_alloc = alloc;
  d->prim_native_name = "";
  d->prim_native_repr_args = slice(std::vector<NativeRepr>(static_cast<std::size_t>(arity), NativeRepr{}));
  d->prim_native_repr_res = NativeRepr{};
  return d;
}

// Obj tags
constexpr long forward_tag = 250;
constexpr long lazy_tag = 246;
constexpr long forcing_tag = 244;

// ---- patterns.ml -----------------------------------------------------------------------
using Pat = const tt::Pattern*;
using Pats = std::vector<Pat>;

template <class D>
const D* mkd(D d) {
  return make<D>(std::move(d));
}
Pat with_desc(Pat p, const tt::PatternDesc* d) {
  tt::Pattern* q = make<tt::Pattern>(*p);
  q->pat_desc = d;
  return q;
}

// Patterns.omega
Pat omega() {
  static Pat o = [] {
    ZoneScope perm(permanent_zone());
    return make<tt::Pattern>(mkd(tt::Tpat_any{{PK::Tpat_any}}), location::none(), Slice<tt::PatExtraItem>{},
                             ctype::none(), env::empty(), tt::Attributes{});
  }();
  return o;
}
Pats omegas(long i) { return Pats(i > 0 ? static_cast<std::size_t>(i) : 0, omega()); }
template <class Lst>
Pats omega_list(const Lst& l) {
  return Pats(l.size(), omega());
}
Pats append(Pats a, const Pats& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
Pats cons(Pat p, const Pats& l) {
  Pats r;
  r.reserve(l.size() + 1);
  r.push_back(p);
  r.insert(r.end(), l.begin(), l.end());
  return r;
}

// Patterns.Head
enum class HK { Any, Construct, Constant, Tuple, Record, Variant, Array, Lazy };
struct Head {
  HK kind;
  const ConstructorDescription* cstr = nullptr;  // Construct
  tt::Constant c{};                              // Constant
  std::vector<OptStr> tuple;                     // Tuple (the labels)
  std::vector<const LabelDescription*> lbls;     // Record
  std::string_view tag;                          // Variant
  bool has_arg = false;                          // Variant
  tt::RowDescRef* cstr_row = nullptr;            // Variant
  MutableFlag am = MutableFlag::Immutable;       // Array
  long n = 0;                                    // Array
  // pattern_data
  Location pat_loc;
  Slice<tt::PatExtraItem> pat_extra;
  TypeExpr* pat_type = nullptr;
  env::t pat_env = nullptr;
  tt::Attributes pat_attributes;
};
using HeadP = const Head*;

Head* head_of(Pat q, HK k) {
  Head* h = make<Head>();
  h->kind = k;
  h->pat_loc = q->pat_loc;
  h->pat_extra = q->pat_extra;
  h->pat_type = q->pat_type;
  h->pat_env = q->pat_env;
  h->pat_attributes = q->pat_attributes;
  return h;
}
// Head.omega
HeadP head_omega() {
  static HeadP h = [] {
    ZoneScope perm(permanent_zone());
    return head_of(omega(), HK::Any);
  }();
  return h;
}
struct Deconstructed {
  HeadP head;
  Pats args;
};
// [deconstruct p] returns the head of [p] and the list of sub patterns.
Deconstructed deconstruct(Pat q) {
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
    default: fatal_error("Patterns.Head.deconstruct: not a simple pattern");
  }
}
long head_arity(HeadP t) {
  switch (t->kind) {
    case HK::Any:
    case HK::Constant: return 0;
    case HK::Construct: return t->cstr->cstr_arity;
    case HK::Tuple: return static_cast<long>(t->tuple.size());
    case HK::Array: return t->n;
    case HK::Record: return static_cast<long>(t->lbls.size());
    case HK::Variant: return t->has_arg ? 1 : 0;
    case HK::Lazy: return 1;
  }
  return 0;
}
// reconstructs a pattern, putting wildcards as sub-patterns.
Pat to_omega_pattern(HeadP t) {
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
      pat_desc =
          mkd(tt::Tpat_construct{{PK::Tpat_construct}, lid_loc, t->cstr, slice(omegas(t->cstr->cstr_arity)), nullptr});
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
    default: fatal_error("to_omega_pattern");
  }
  return make<tt::Pattern>(pat_desc, t->pat_loc, Slice<tt::PatExtraItem>{}, t->pat_type, t->pat_env,
                           t->pat_attributes);
}

// ---- Parmatch helpers --------------------------------------------------------------------
// MayCompat = Parmatch.Compat (Data_types.may_equal_constr)
bool may_compat(Pat p, Pat q) { return parmatch::compat_with(data_types::may_equal_constr, p, q); }
bool may_compats(const Pats& ps, const Pats& qs) {
  std::size_t k = 0;
  for (; k < ps.size() && k < qs.size(); ++k)
    if (!may_compat(ps[k], qs[k])) return false;
  return ps.size() == qs.size();
}
// Parmatch.get_mins
template <class T, class Le>
std::vector<T> get_mins(const Le& le, const std::vector<T>& ps) {
  auto select_rec = [&](const std::vector<T>& l) {
    std::vector<T> r;  // built by consing: r's head is its last element
    for (std::size_t i = 0; i < l.size(); ++i) {
      bool smaller_follows = false;
      for (std::size_t j = i + 1; j < l.size() && !smaller_follows; ++j)
        if (le(l[j], l[i])) smaller_follows = true;
      if (!smaller_follows) r.push_back(l[i]);
    }
    return std::vector<T>(r.rbegin(), r.rend());
  };
  return select_rec(select_rec(ps));
}

// ---- matching.ml ---------------------------------------------------------------------------
std::vector<tt::RecordPatField> all_record_args(Slice<tt::RecordPatField> lbls) {
  if (lbls.empty()) fatal_error("Matching.all_record_args");
  auto lbl_all = lbls[0].label->lbl_all;
  std::vector<tt::RecordPatField> t;
  for (auto* lbl : lbl_all) t.push_back({tt::LidLoc{Longident::lident("?temp?"), location::none()}, lbl, omega()});
  for (auto& x : lbls) t[static_cast<std::size_t>(x.label->lbl_pos)] = x;
  return t;
}

HeadP expand_record_head(HeadP h) {
  if (h->kind != HK::Record) return h;
  if (h->lbls.empty()) fatal_error("Matching.expand_record_head");
  Head* r = make<Head>(*h);
  auto lbl_all = h->lbls[0]->lbl_all;
  r->lbls.assign(lbl_all.begin(), lbl_all.end());
  return r;
}

lam bind_alias(Pat p, Ident::t id, lam arg, lam action) {
  L::ValueKind k = typeopt::value_kind(p->pat_env, p->pat_type);
  return L::bind_with_value_kind(LetKind::Alias, id, k, arg, action);
}

L::ScopedLocation head_loc(scopes sc, HeadP head) { return debuginfo::of_location(sc, head->pat_loc); }

// 'a clause with a Non_empty_row: ((p, ps), action)
struct Clause {
  Pat p;
  Pats ps;
  lam act;
};
// initial_clause = pattern list clause
struct InitialClause {
  Pats ps;
  lam act;
};
using Matrix = std::vector<Pats>;

// Non_empty_row.of_initial
Clause of_initial(const InitialClause& c) {
  if (c.ps.empty()) fatal_error("Non_empty_row.of_initial");
  return {c.ps[0], Pats(c.ps.begin() + 1, c.ps.end()), c.act};
}

bool is_simple_view(Pat p) {
  switch (p->pat_desc->kind) {
    case PK::Tpat_any:
    case PK::Tpat_constant:
    case PK::Tpat_tuple:
    case PK::Tpat_construct:
    case PK::Tpat_variant:
    case PK::Tpat_record:
    case PK::Tpat_array:
    case PK::Tpat_lazy: return true;
    default: return false;
  }
}

// ---- Half_simple ----
Pat simpl_under_orpat(Pat p) {
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any:
    case PK::Tpat_var: return p;
    case PK::Tpat_alias: {
      auto* a = as<tt::Tpat_alias>(d);
      return with_desc(p, mkd(tt::Tpat_alias{{PK::Tpat_alias}, simpl_under_orpat(a->pat), a->id, a->name, a->uid, a->ty}));
    }
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      // `let p1, p2 = (simpl p1, simpl p2)`: the tuple right to left
      Pat p2 = simpl_under_orpat(o->p2);
      Pat p1 = simpl_under_orpat(o->p1);
      if (parmatch::le_pat(p1, p2)) return p1;
      return with_desc(p, mkd(tt::Tpat_or{{PK::Tpat_or}, p1, p2, o->row}));
    }
    case PK::Tpat_record: {
      auto* r = as<tt::Tpat_record>(d);
      return with_desc(p, mkd(tt::Tpat_record{{PK::Tpat_record}, slice(all_record_args(r->fields)), r->closed}));
    }
    default: return p;
  }
}

// Explode or-patterns and turn aliases into bindings in actions
Clause half_simple_of_clause(lam arg, Clause cl) {
  for (;;) {
    Pat p = cl.p;
    const tt::PatternDesc* d = p->pat_desc;
    switch (d->kind) {
      case PK::Tpat_any: return cl;
      case PK::Tpat_var: {
        auto* v = as<tt::Tpat_var>(d);
        cl.p = with_desc(p, mkd(tt::Tpat_alias{{PK::Tpat_alias}, omega(), v->id, v->name, v->uid, p->pat_type}));
        continue;
      }
      case PK::Tpat_alias: {
        auto* a = as<tt::Tpat_alias>(d);
        cl.act = bind_alias(a->pat, a->id, arg, cl.act);
        cl.p = a->pat;
        continue;
      }
      case PK::Tpat_record: {
        auto* r = as<tt::Tpat_record>(d);
        if (r->fields.empty()) return cl;
        cl.p = with_desc(p, mkd(tt::Tpat_record{{PK::Tpat_record}, slice(all_record_args(r->fields)), r->closed}));
        return cl;
      }
      case PK::Tpat_or: {
        Pat orpat = simpl_under_orpat(p);
        cl.p = orpat;
        if (orpat->pat_desc->kind == PK::Tpat_or) return cl;
        continue;
      }
      case PK::Tpat_constant:
      case PK::Tpat_tuple:
      case PK::Tpat_construct:
      case PK::Tpat_variant:
      case PK::Tpat_array:
      case PK::Tpat_lazy: return cl;
      default: fatal_error("Matching: not a value pattern");
    }
  }
}

// ---- Simple ----
HeadP simple_head(Pat p) { return deconstruct(p).head; }

Pat simple_alpha(const std::vector<std::pair<Ident::t, Ident::t>>& env, Pat p) {
  auto alpha_pat = [&](Pat q) { return tt::alpha_pat(env, q); };
  const tt::PatternDesc* d = p->pat_desc;
  const tt::PatternDesc* pat_desc;
  switch (d->kind) {
    case PK::Tpat_any: pat_desc = mkd(tt::Tpat_any{{PK::Tpat_any}}); break;
    case PK::Tpat_constant: pat_desc = mkd(tt::Tpat_constant{{PK::Tpat_constant}, as<tt::Tpat_constant>(d)->c}); break;
    case PK::Tpat_tuple: {
      std::vector<tt::LabeledPattern> l;
      for (auto& x : as<tt::Tpat_tuple>(d)->pats) l.push_back({x.label, alpha_pat(x.pat)});
      pat_desc = mkd(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(l)});
      break;
    }
    case PK::Tpat_construct: {
      auto* c = as<tt::Tpat_construct>(d);
      Pats args;
      for (Pat a : c->args) args.push_back(alpha_pat(a));
      pat_desc = mkd(tt::Tpat_construct{{PK::Tpat_construct}, c->lid, c->cstr, slice(args), nullptr});
      break;
    }
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      pat_desc = mkd(tt::Tpat_variant{{PK::Tpat_variant}, v->label, v->arg ? alpha_pat(v->arg) : nullptr, v->row});
      break;
    }
    case PK::Tpat_record: {
      auto* r = as<tt::Tpat_record>(d);
      std::vector<tt::RecordPatField> l;
      for (auto& f : r->fields) l.push_back({f.lid, f.label, alpha_pat(f.pat)});
      pat_desc = mkd(tt::Tpat_record{{PK::Tpat_record}, slice(l), r->closed});
      break;
    }
    case PK::Tpat_array: {
      auto* a = as<tt::Tpat_array>(d);
      Pats ps;
      for (Pat x : a->pats) ps.push_back(alpha_pat(x));
      pat_desc = mkd(tt::Tpat_array{{PK::Tpat_array}, a->mut, slice(ps)});
      break;
    }
    case PK::Tpat_lazy: pat_desc = mkd(tt::Tpat_lazy{{PK::Tpat_lazy}, alpha_pat(as<tt::Tpat_lazy>(d)->pat)}); break;
    default: fatal_error("Matching.Simple.alpha");
  }
  return with_desc(p, pat_desc);
}

using MkAction = std::function<lam(const std::vector<Ident::t>&)>;

struct PatAct {
  Pat p;
  lam act;
};

// Simple.explode_or_pat: the clauses are returned head first
std::vector<PatAct> explode_or_pat(lam arg, Pat p0, const MkAction& mk_action,
                                   const std::vector<Ident::t>& patbound_action_vars) {
  std::function<std::vector<PatAct>(Pat, const std::vector<Ident::t>&, std::vector<PatAct>)> explode;
  explode = [&](Pat p, const std::vector<Ident::t>& aliases, std::vector<PatAct> rem) -> std::vector<PatAct> {
    const tt::PatternDesc* d = p->pat_desc;
    switch (d->kind) {
      case PK::Tpat_or: {
        auto* o = as<tt::Tpat_or>(d);
        std::vector<PatAct> r2 = explode(o->p2, aliases, std::move(rem));
        return explode(o->p1, aliases, std::move(r2));
      }
      case PK::Tpat_alias: {
        auto* a = as<tt::Tpat_alias>(d);
        std::vector<Ident::t> aliases2{a->id};
        aliases2.insert(aliases2.end(), aliases.begin(), aliases.end());
        return explode(a->pat, aliases2, std::move(rem));
      }
      case PK::Tpat_var: {
        auto* v = as<tt::Tpat_var>(d);
        return explode(with_desc(p, mkd(tt::Tpat_alias{{PK::Tpat_alias}, omega(), v->id, v->name, v->uid, p->pat_type})),
                       aliases, std::move(rem));
      }
      default: break;
    }
    // We freshen the variables of the pattern, and bind the variables in
    // [aliases] to the argument [arg], binding [arg] itself only if needed.
    auto mem = [&](Ident::t id) {
      for (Ident::t a : aliases)
        if (ident::same(a, id)) return true;
      return false;
    };
    std::function<PatAct(Ident::t, std::vector<Ident::t>, std::vector<std::pair<Ident::t, Ident::t>>, std::size_t)>
        fresh_clause;
    fresh_clause = [&](Ident::t arg_id, std::vector<Ident::t> action_vars,
                       std::vector<std::pair<Ident::t, Ident::t>> renaming_env, std::size_t k) -> PatAct {
      if (k == patbound_action_vars.size()) {
        Pat fresh_pat = simple_alpha(renaming_env, p);
        std::vector<Ident::t> vars(action_vars.rbegin(), action_vars.rend());
        lam fresh_action = mk_action(vars);
        return {fresh_pat, fresh_action};
      }
      Ident::t pat_id = patbound_action_vars[k];
      if (!mem(pat_id)) {
        Ident::t fresh_id = ident::rename(pat_id);
        action_vars.insert(action_vars.begin(), fresh_id);
        renaming_env.insert(renaming_env.begin(), {pat_id, fresh_id});
        return fresh_clause(arg_id, std::move(action_vars), std::move(renaming_env), k + 1);
      }
      Ident::t id = arg_id;
      if (!id)
        if (auto* v = L::as<L::Lvar>(arg)) id = v->id;
      if (id) {
        action_vars.insert(action_vars.begin(), id);
        return fresh_clause(arg_id, std::move(action_vars), std::move(renaming_env), k + 1);
      }
      // [pat_id] is a name used locally to refer to the argument, so it
      // makes sense to reuse it (refreshed)
      Ident::t nid = ident::rename(pat_id);
      action_vars.insert(action_vars.begin(), nid);
      PatAct pa = fresh_clause(nid, std::move(action_vars), std::move(renaming_env), k + 1);
      return {pa.p, bind_alias(pa.p, nid, arg, pa.act)};
    };
    PatAct c = fresh_clause(nullptr, {}, {}, 0);
    rem.insert(rem.begin(), c);
    return rem;
  };
  return explode(p0, {}, {});
}

Pat expand_record_simple(Pat p) {
  if (auto* r = as<tt::Tpat_record>(p->pat_desc))
    return with_desc(p, mkd(tt::Tpat_record{{PK::Tpat_record}, slice(all_record_args(r->fields)), ClosedFlag::Closed}));
  return p;
}

Matrix add_omega_column(const Matrix& pss) {
  Matrix r;
  for (auto& ps : pss) r.push_back(cons(omega(), ps));
  return r;
}

// rev_split_at n ps = (the first n elements, the rest)
std::pair<Pats, Pats> rev_split_at(long n, const Pats& ps) {
  if (n < 0) n = 0;
  if (static_cast<std::size_t>(n) > ps.size()) fatal_error("Matching.rev_split_at");
  return {Pats(ps.begin(), ps.begin() + n), Pats(ps.begin() + n, ps.end())};
}

// matcher discr p rem (None = NoMatch)
std::optional<Pats> matcher(HeadP discr0, Pat p0, const Pats& rem) {
  HeadP discr = expand_record_head(discr0);
  Pat p = expand_record_simple(p0);
  auto [ph, args] = deconstruct(p);
  auto yes = [&]() -> std::optional<Pats> { return append(args, rem); };
  auto yesif = [&](bool b) -> std::optional<Pats> {
    if (b) return yes();
    return std::nullopt;
  };
  if (discr->kind == HK::Any) return rem;
  if (ph->kind == HK::Any) return append(omegas(head_arity(discr)), rem);
  if (discr->kind != ph->kind) return std::nullopt;
  switch (discr->kind) {
    case HK::Constant: return yesif(parmatch::const_compare(discr->c, ph->c) == 0);
    // NB: may_equal_constr considers (potential) constructor rebinding
    case HK::Construct: return yesif(data_types::may_equal_constr(discr->cstr, ph->cstr));
    case HK::Variant: return yesif(discr->tag == ph->tag && discr->has_arg == ph->has_arg);
    case HK::Array: return yesif(discr->am == ph->am && discr->n == ph->n);
    case HK::Tuple: return yesif(discr->tuple == ph->tuple);
    // we already expanded the record fully
    case HK::Record: return yesif(discr->lbls.size() == ph->lbls.size());
    case HK::Lazy: return yes();
    case HK::Any: break;
  }
  return std::nullopt;
}

long ncols(const Matrix& pss) { return pss.empty() ? 0 : static_cast<long>(pss[0].size()); }

// ---- Context ----
struct CtxRow {
  Pats left;
  Pats right;
};
using Context = std::vector<CtxRow>;

namespace row {
bool le(const CtxRow& c1, const CtxRow& c2) {
  return parmatch::le_pats(c1.left, c2.left) && parmatch::le_pats(c1.right, c2.right);
}
CtxRow lshift(const CtxRow& r) {
  if (r.right.empty()) fatal_error("Matching.Context.lshift");
  return {cons(r.right[0], r.left), Pats(r.right.begin() + 1, r.right.end())};
}
CtxRow lforget(const CtxRow& r) {
  if (r.right.empty()) fatal_error("Matching.Context.lforget");
  return {cons(omega(), r.left), Pats(r.right.begin() + 1, r.right.end())};
}
CtxRow erase_first_col(const CtxRow& r) {
  if (r.right.empty()) fatal_error("Matching.Context.erase_first_col");
  return {r.left, cons(omega(), Pats(r.right.begin() + 1, r.right.end()))};
}
CtxRow rshift(const CtxRow& r) {
  if (r.left.empty()) fatal_error("Matching.Context.rshift");
  return {Pats(r.left.begin() + 1, r.left.end()), cons(r.left[0], r.right)};
}
CtxRow rshift_num(long n, const CtxRow& r) {
  auto [shifted, left] = rev_split_at(n, r.left);
  return {left, append(shifted, r.right)};
}
// { (_,_)::left; p1::p2::right } -> { left; (p1,p2)::right }
CtxRow combine(const CtxRow& r) {
  if (r.left.empty()) fatal_error("Matching.Context.combine");
  return {Pats(r.left.begin() + 1, r.left.end()), parmatch::set_args(r.left[0], r.right)};
}
}  // namespace row

namespace context {
Context empty() { return {}; }  // Context.empty
Context start(long n) { return {CtxRow{{}, omegas(n)}}; }
bool is_empty(const Context& c) { return c.empty(); }
template <class F>
Context map(F f, const Context& ctx) {
  Context r;
  for (auto& x : ctx) r.push_back(f(x));
  return r;
}
Context lshift(const Context& ctx) {
  if (static_cast<long>(ctx.size()) < match_context_rows) return map(row::lshift, ctx);
  // Context pruning
  return get_mins(row::le, map(row::lforget, ctx));
}
Context rshift(const Context& ctx) { return map(row::rshift, ctx); }
Context erase_first_col(const Context& ctx) { return map(row::erase_first_col, ctx); }
Context rshift_num(long n, const Context& ctx) {
  return map([n](const CtxRow& r) { return row::rshift_num(n, r); }, ctx);
}
Context combine(const Context& ctx) { return map(row::combine, ctx); }

Context specialize(HeadP head, const Context& ctx) {
  struct Item {
    Pats left;
    Pat p;
    Pats right;
  };
  std::deque<Item> work;
  for (auto& r : ctx) {
    if (r.right.empty()) fatal_error("Matching.Context.specialize");
    work.push_back({r.left, r.right[0], Pats(r.right.begin() + 1, r.right.end())});
  }
  Context out;
  while (!work.empty()) {
    Item it = std::move(work.front());
    work.pop_front();
    const tt::PatternDesc* d = it.p->pat_desc;
    if (auto* o = as<tt::Tpat_or>(d)) {
      work.push_front({it.left, o->p2, it.right});
      work.push_front({it.left, o->p1, it.right});
      continue;
    }
    if (auto* a = as<tt::Tpat_alias>(d)) {
      it.p = a->pat;
      work.push_front(std::move(it));
      continue;
    }
    if (d->kind == PK::Tpat_var) {
      it.p = omega();
      work.push_front(std::move(it));
      continue;
    }
    std::optional<Pats> right = matcher(head, it.p, it.right);
    if (!right) continue;
    out.push_back({cons(to_omega_pattern(head), it.left), std::move(*right)});
  }
  return out;
}

Context select_columns(const Matrix& pss, const Context& ctx) {
  long n = ncols(pss);
  Context out;
  for (auto& ps : pss)
    for (auto& r : ctx) {
      auto [transfer, right] = rev_split_at(n, r.right);
      Pats inter;
      try {
        inter = parmatch::lubs(transfer, ps);
      } catch (const parmatch::Empty&) {
        continue;
      }
      out.push_back({append(inter, r.left), right});
    }
  return out;
}

Context lub(Pat p, const Context& ctx) {
  Context out;
  for (auto& r : ctx) {
    if (r.right.empty()) fatal_error("Matching.Context.lub");
    Pat l;
    try {
      l = parmatch::lub(p, r.right[0]);
    } catch (const parmatch::Empty&) {
      continue;
    }
    out.push_back({r.left, cons(l, Pats(r.right.begin() + 1, r.right.end()))});
  }
  return out;
}

bool matches(const Context& ctx, const Matrix& pss) {
  for (auto& r : ctx)
    for (auto& ps : pss)
      if (may_compats(r.right, ps)) return true;
  return false;
}

Context union_(const Context& pss, const Context& qss) {
  Context all = pss;
  all.insert(all.end(), qss.begin(), qss.end());
  return get_mins(row::le, all);
}
}  // namespace context

// flatten_matrix size pss
void flatten_pat_line(long size, Pat p, Matrix& out) {
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any:
    case PK::Tpat_var: out.push_back(omegas(size)); return;
    case PK::Tpat_tuple: {
      Pats ps;
      for (auto& x : as<tt::Tpat_tuple>(d)->pats) ps.push_back(x.pat);
      out.push_back(ps);
      return;
    }
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      flatten_pat_line(size, o->p1, out);
      flatten_pat_line(size, o->p2, out);
      return;
    }
    case PK::Tpat_alias: flatten_pat_line(size, as<tt::Tpat_alias>(d)->pat, out); return;
    default: fatal_error("Matching.flatten_pat_line");
  }
}
Matrix flatten_matrix(long size, const Matrix& pss) {
  Matrix out;
  for (auto& ps : pss) {
    if (ps.size() != 1) fatal_error("Matching.flatten_matrix");
    flatten_pat_line(size, ps[0], out);
  }
  return out;
}

// ---- Default_environment ----
struct DefaultEnv {
  std::vector<std::pair<long, Matrix>> env;
  long final_exit = 0;
};
using RowMatcher = std::function<std::optional<Pats>(Pat, const Pats&)>;

namespace default_env {
DefaultEnv empty(long final_exit) { return {{}, final_exit}; }
lam raise_final_exit(const DefaultEnv& d) { return lstaticraise(d.final_exit); }
DefaultEnv cons_(const Matrix& matrix, long raise_num, const DefaultEnv& def) {
  if (matrix.empty()) return def;
  DefaultEnv r = def;
  r.env.insert(r.env.begin(), {raise_num, matrix});
  return r;
}

struct PRow {
  Pat p;
  Pats ps;
};
Matrix specialize_matrix(long arity, const RowMatcher& m, const std::vector<PRow>& pss);

Matrix filter_rows(long arity, const RowMatcher& m, std::deque<PRow> work) {
  Matrix out;
  while (!work.empty()) {
    PRow r = std::move(work.front());
    work.pop_front();
    const tt::PatternDesc* d = r.p->pat_desc;
    if (auto* a = as<tt::Tpat_alias>(d)) {
      r.p = a->pat;
      work.push_front(std::move(r));
      continue;
    }
    if (d->kind == PK::Tpat_var) {
      r.p = omega();
      work.push_front(std::move(r));
      continue;
    }
    if (auto* o = as<tt::Tpat_or>(d)) {
      // filter_rec_or p1 p2 ps rem
      Pat p1 = o->p1, p2 = o->p2;
      auto filter_one = [&](Pat p) { return filter_rows(arity, m, std::deque<PRow>{PRow{p, r.ps}}); };
      if (arity == 0) {
        // if K has arity 0, specializing ((K|K)::rem) returns just (rem)
        Matrix matches = filter_one(p1);
        if (matches.empty()) {
          work.push_front({p2, r.ps});
        } else {
          for (auto& x : matches) out.push_back(std::move(x));
        }
        continue;
      }
      if (arity == 1) {
        // ((K p | K q) :: rem) as ((p | q) :: rem); the pair right to left
        Matrix row2 = filter_one(p2);
        Matrix row1 = filter_one(p1);
        if (row1.empty() || row2.empty()) {
          for (auto& x : (row1.empty() ? row2 : row1)) out.push_back(std::move(x));
          continue;
        }
        if (row1.size() > 1 || row2.size() > 1) fatal_error("Matching.filter_rec_or");
        if (row1[0].empty() || row2[0].empty()) fatal_error("Matching.filter_rec_or");
        Pat arg1 = row1[0][0], arg2 = row2[0][0];
        tt::Pattern* orp = make<tt::Pattern>(*arg1);
        orp->pat_desc = mkd(tt::Tpat_or{{PK::Tpat_or}, arg1, arg2, nullptr});
        orp->pat_loc = location::none();
        out.push_back(cons(orp, r.ps));
        continue;
      }
      // (K (p1, .., pn) | K (q1, .. qn)) is not (p1 .. pn | q1 .. qn)
      work.push_front({p2, r.ps});
      work.push_front({p1, r.ps});
      continue;
    }
    std::optional<Pats> specialized = m(r.p, r.ps);
    if (!specialized) continue;
    if (specialized->size() != r.ps.size() + static_cast<std::size_t>(arity))
      fatal_error("Matching.Default_environment.specialize_matrix");
    out.push_back(std::move(*specialized));
  }
  return out;
}
Matrix specialize_matrix(long arity, const RowMatcher& m, const std::vector<PRow>& pss) {
  return filter_rows(arity, m, std::deque<PRow>(pss.begin(), pss.end()));
}

DefaultEnv specialize_(long arity, const RowMatcher& m, const DefaultEnv& def) {
  std::vector<std::pair<long, Matrix>> out;
  for (auto& [i, pss] : def.env) {
    if (!pss.empty() && pss[0].empty()) {
      out.push_back({i, Matrix{Pats{}}});
      break;
    }
    // all rows in pss are non-empty
    std::vector<PRow> rows;
    for (auto& ps : pss) {
      if (ps.empty()) fatal_error("Matching.Default_environment.specialize_");
      rows.push_back({ps[0], Pats(ps.begin() + 1, ps.end())});
    }
    Matrix r = specialize_matrix(arity, m, rows);
    if (r.empty()) continue;
    if (r[0].empty()) {
      out.push_back({i, Matrix{Pats{}}});
      break;
    }
    out.push_back({i, std::move(r)});
  }
  return {std::move(out), def.final_exit};
}

DefaultEnv specialize(HeadP head, const DefaultEnv& def) {
  return specialize_(head_arity(head), [head](Pat p, const Pats& rem) { return matcher(head, p, rem); }, def);
}
DefaultEnv pop_column(const DefaultEnv& def) {
  return specialize_(0, [](Pat, const Pats& rem) -> std::optional<Pats> { return rem; }, def);
}
DefaultEnv pop_compat(Pat p, const DefaultEnv& def) {
  return specialize_(
      0,
      [p](Pat q, const Pats& rem) -> std::optional<Pats> {
        if (may_compat(p, q)) return rem;
        return std::nullopt;
      },
      def);
}
std::optional<std::pair<std::pair<long, Matrix>, DefaultEnv>> pop(const DefaultEnv& def) {
  if (def.env.empty()) return std::nullopt;
  DefaultEnv rest{std::vector<std::pair<long, Matrix>>(def.env.begin() + 1, def.env.end()), def.final_exit};
  return std::make_pair(def.env[0], std::move(rest));
}
DefaultEnv flatten(long size, const DefaultEnv& def) {
  DefaultEnv r{{}, def.final_exit};
  for (auto& [i, pss] : def.env) r.env.push_back({i, flatten_matrix(size, pss)});
  return r;
}
}  // namespace default_env

// ---- Jumps ----
struct Jumps {
  std::vector<std::pair<long, Context>> env;  // decreasing exit numbers
  Partial partial;
};
namespace jumps {
Partial partial(const Jumps& j) { return j.partial; }
std::pair<Context, Jumps> extract(long i, const Jumps& jumps) {
  Jumps rest{{}, jumps.partial};
  Context ctx = context::empty();
  std::size_t k = 0;
  for (; k < jumps.env.size(); ++k) {
    long j = jumps.env[k].first;
    if (i == j) {
      ctx = jumps.env[k].second;
      ++k;
      break;
    }
    if (j < i) break;
    rest.env.push_back(jumps.env[k]);
  }
  for (; k < jumps.env.size(); ++k) rest.env.push_back(jumps.env[k]);
  return {ctx, rest};
}
Jumps remove(long i, const Jumps& jumps) {
  Jumps r{{}, jumps.partial};
  bool removed = false;
  for (auto& x : jumps.env) {
    if (!removed && x.first == i) {
      removed = true;
      continue;
    }
    r.env.push_back(x);
  }
  return r;
}
Jumps empty(Partial p) { return {{}, p}; }
Jumps add(long i, const Context& ctx, const Jumps& jumps) {
  if (context::is_empty(ctx)) return jumps;
  Jumps r{{}, jumps.partial};
  std::size_t k = 0;
  bool done = false;
  for (; k < jumps.env.size(); ++k) {
    auto& [j, qss] = jumps.env[k];
    if (j > i) {
      r.env.push_back(jumps.env[k]);
      continue;
    }
    if (j < i) {
      r.env.push_back({i, ctx});
    } else {
      r.env.push_back({i, context::union_(ctx, qss)});
      ++k;
    }
    done = true;
    break;
  }
  if (!done) r.env.push_back({i, ctx});
  for (; k < jumps.env.size(); ++k) r.env.push_back(jumps.env[k]);
  return r;
}
// Total: a singleton only jumps to exit [i], not to the final exit.
Jumps singleton(long i, const Context& ctx) { return add(i, ctx, empty(Partial::Total)); }
Jumps union_(const Jumps& j1, const Jumps& j2) {
  Jumps r;
  std::size_t a = 0, b = 0;
  auto& e1 = j1.env;
  auto& e2 = j2.env;
  while (a < e1.size() && b < e2.size()) {
    long i1 = e1[a].first, i2 = e2[b].first;
    if (i1 == i2) {
      r.env.push_back({i1, context::union_(e1[a].second, e2[b].second)});
      ++a;
      ++b;
    } else if (i1 > i2) {
      r.env.push_back(e1[a++]);
    } else {
      r.env.push_back(e2[b++]);
    }
  }
  for (; a < e1.size(); ++a) r.env.push_back(e1[a]);
  for (; b < e2.size(); ++b) r.env.push_back(e2[b]);
  r.partial = j1.partial == Partial::Total && j2.partial == Partial::Total ? Partial::Total : Partial::Partial;
  return r;
}
std::vector<Jumps> merge(const std::vector<Jumps>& envs) {
  std::vector<Jumps> r;
  std::size_t k = 0;
  for (; k + 1 < envs.size(); k += 2) r.push_back(union_(envs[k], envs[k + 1]));
  for (; k < envs.size(); ++k) r.push_back(envs[k]);
  return r;
}
Jumps unions(std::vector<Jumps> envs) {
  for (;;) {
    if (envs.empty()) return empty(Partial::Total);
    if (envs.size() == 1) return envs[0];
    envs = merge(envs);
  }
}
template <class F>
Jumps map(F f, const Jumps& jumps) {
  Jumps r{{}, jumps.partial};
  for (auto& [i, pss] : jumps.env) r.env.push_back({i, f(pss)});
  return r;
}
}  // namespace jumps

// ---- temporality, partiality ----
enum class Temporality { First, Following };
struct Partiality {
  Partial current;
  Partial global;
  Temporality tempo;
};
// arg_partiality = Arg of partiality
struct ArgPartiality {
  Partiality p;
};

// ---- pattern matching before any compilation ----
struct ArgT {  // lambda arg
  lam arg;
  LetKind binding_kind;
  MutableFlag mut;
};
using Args = std::vector<ArgT>;
// pure_arg = Var of Ident.t | Tuple of lambda
struct PureArg {
  Ident::t var = nullptr;  // Var
  lam tuple = nullptr;     // Tuple
};
struct FirstArg {
  PureArg arg;
  LetKind binding_kind;
  MutableFlag mut;
};
struct SplitArgs {
  FirstArg first;
  Args rest;
};
lam arg_of_pure(const PureArg& a) { return a.var ? L::lvar(a.var) : a.tuple; }

template <class A, class C>
struct PM {  // ('args, 'row) pattern_matching
  std::vector<C> cases;
  A args;
  DefaultEnv def;
};
using InitialPM = PM<Args, InitialClause>;

struct Handler {
  Matrix provenance;
  long exit;
  std::vector<L::Param> vars;
  InitialPM pm;
};

MutableFlag compose_mut(MutableFlag m1, MutableFlag m2) {
  if (m1 == MutableFlag::Immutable && m2 == MutableFlag::Immutable) return MutableFlag::Immutable;
  return MutableFlag::Mutable;
}

// pm_half_compiled
struct PmHalf;
using PmHalfP = std::shared_ptr<const PmHalf>;
using SimplePM = PM<SplitArgs, Clause>;
struct PmHalf {
  enum class K { PmOr, PmVar, Pm } kind;
  SimplePM pm;                    // Pm; PmOr's body
  std::vector<Handler> handlers;  // PmOr
  Matrix or_matrix;               // PmOr
  PmHalfP inside;                 // PmVar
};
struct PmInfo {  // pm_half_compiled_info
  PmHalfP me;
  Matrix matrix;
  DefaultEnv top_default;
};
using Nexts = std::vector<std::pair<long, PmHalfP>>;

// ---- action sharing ----
// StoreExp = Switch.Store (lambda keys, Stdlib.compare as the order)
struct StoredLambda {
  using t = lam;
  using key = lam;
  static std::optional<key> make_key(const t& l) { return L::make_key(l); }
  static bool same_key(const key& a, const key& b) { return L::equal_lambda(a, b); }
};
using StoreExp = switch_::Store<StoredLambda>;

lam make_exit(long i) { return lstaticraise(i); }

// Introduce a catch, if worth it
lam make_catch(lam d, const std::function<lam(lam)>& k) {
  if (auto* r = L::as<L::Lstaticraise>(d); r && r->args.empty()) return k(d);
  long e = L::next_raise_count();
  return L::lstaticcatch(k(make_exit(e)), e, {}, d);
}

// Introduce a catch, if worth it, delayed version
std::optional<long> as_simple_exit(lam l) {
  for (;;) {
    if (auto* r = L::as<L::Lstaticraise>(l); r && r->args.empty()) return r->i;
    if (auto* lt = L::as<L::Llet>(l); lt && lt->str == LetKind::Alias) {
      l = lt->body;
      continue;
    }
    return std::nullopt;
  }
}

std::pair<long, std::function<lam(lam)>> make_catch_delayed(lam handler) {
  if (auto i = as_simple_exit(handler)) return {*i, [](lam act) { return act; }};
  long i = L::next_raise_count();
  return {i, [i, handler](lam body) -> lam {
            if (auto* r = L::as<L::Lstaticraise>(body)) return i == r->i ? handler : body;
            return L::lstaticcatch(body, i, {}, handler);
          }};
}

lam raw_action(lam l) {
  if (auto k = L::make_key(l)) return *k;
  return l;
}

bool eq_key_opt(const std::optional<lam>& a, const std::optional<lam>& b) {
  if (!a || !b) return !a && !b;
  return L::equal_lambda(*a, *b);
}

lam same_actions(const std::vector<lam>& acts) {  // nullptr = None
  if (acts.empty()) return nullptr;
  if (acts.size() == 1) return acts[0];
  std::optional<lam> key0 = L::make_key(acts[0]);
  if (!key0) return nullptr;
  for (std::size_t k = 1; k < acts.size(); ++k)
    if (!eq_key_opt(L::make_key(acts[k]), key0)) return nullptr;
  return acts[0];
}
template <class K>
lam same_actions(const std::vector<std::pair<K, lam>>& l) {
  std::vector<lam> acts;
  for (auto& [_, a] : l) acts.push_back(a);
  return same_actions(acts);
}

// Test for swapping two clauses
bool safe_before(const Clause& cl, const std::vector<Clause>& l) {
  auto same_actions2 = [](lam act1, lam act2) {
    std::optional<lam> k1 = L::make_key(act1);
    std::optional<lam> k2 = L::make_key(act2);
    return k1 && k2 && L::equal_lambda(*k1, *k2);
  };
  Pats pps = cons(cl.p, cl.ps);
  for (auto& q : l)
    if (!(same_actions2(cl.act, q.act) || !may_compats(pps, cons(q.p, q.ps)))) return false;
  return true;
}

Clause half_simplify_nonempty(lam arg, const Clause& cls) { return half_simple_of_clause(arg, cls); }
Clause half_simplify_clause(lam arg, const InitialClause& cls) { return half_simplify_nonempty(arg, of_initial(cls)); }

// Once matchings are *fully* simplified, one can easily find their nature.
HeadP what_is_cases_(bool skip_any, const std::vector<Clause>& cases) {
  for (auto& c : cases) {
    HeadP head = simple_head(c.p);
    if (head->kind == HK::Any && skip_any) continue;
    return head;
  }
  return head_omega();
}
HeadP what_is_first_case(const std::vector<Clause>& cases) { return what_is_cases_(false, cases); }
HeadP what_is_cases(const std::vector<Clause>& cases) { return what_is_cases_(true, cases); }

L::IdentSet pm_free_variables(const InitialPM& pm) {
  L::IdentSet r;
  for (auto it = pm.cases.rbegin(); it != pm.cases.rend(); ++it) {
    L::IdentSet fv = L::free_variables(it->act);
    r.insert(fv.begin(), fv.end());
  }
  return r;
}

// Basic grouping predicates
bool can_group(HeadP discr, Pat pat) {
  HeadP h = simple_head(pat);
  switch (discr->kind) {
    case HK::Any: return h->kind == HK::Any;
    case HK::Constant: return h->kind == HK::Constant && discr->c.kind == h->c.kind;
    case HK::Construct:
      if (h->kind != HK::Construct) return false;
      if (discr->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension &&
          h->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension)
        // Extension constructors with distinct names may be equal thanks to
        // constructor rebinding.
        return path::same(discr->cstr->cstr_tag.ext_path, h->cstr->cstr_tag.ext_path);
      return true;
    case HK::Tuple: return h->kind == HK::Tuple || h->kind == HK::Any;
    case HK::Record: return h->kind == HK::Record || h->kind == HK::Any;
    case HK::Array: return h->kind == HK::Array;
    case HK::Variant: return h->kind == HK::Variant;
    case HK::Lazy: return h->kind == HK::Lazy;
  }
  return false;
}

bool is_or(Pat p) { return p->pat_desc->kind == PK::Tpat_or; }

bool omega_like(Pat p) {
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any:
    case PK::Tpat_var: return true;
    case PK::Tpat_alias: return omega_like(as<tt::Tpat_alias>(d)->pat);
    case PK::Tpat_or: return omega_like(as<tt::Tpat_or>(d)->p1) || omega_like(as<tt::Tpat_or>(d)->p2);
    default: return false;
  }
}

bool simple_omega_like(Pat p) { return simple_head(p)->kind == HK::Any; }

bool equiv_pat(Pat p, Pat q) { return parmatch::le_pat(p, q) && parmatch::le_pat(q, p); }

// the clauses at the front of [l] whose head is equivalent to [p]
std::pair<std::vector<Clause>, std::vector<Clause>> extract_equiv_head(Pat p, const std::vector<Clause>& l) {
  std::size_t k = 0;
  while (k < l.size() && equiv_pat(p, l[k].p)) ++k;
  return {std::vector<Clause>(l.begin(), l.begin() + k), std::vector<Clause>(l.begin() + k, l.end())};
}

// ---- Or_matrix ----
namespace or_matrix {
bool disjoint(Pat p, Pat q) { return !may_compat(p, q); }
bool safe_below(const Pats& ps, lam act, const Pats& qs) { return !L::is_guarded(act) && parmatch::le_pats(ps, qs); }
bool safe_below_or_matrix(const std::vector<Clause>& l, Pat q, const Pats& qs) {
  for (auto& c : l) {
    if (c.p->pat_desc->kind == PK::Tpat_or) {
      if (!(disjoint(c.p, q) || safe_below(c.ps, c.act, qs))) return false;
    }
  }
  return true;
}
// Insert or append a clause in the Or matrix; if neither is possible, add
// it to the bottom of the No matrix.  rev_ors / rev_no are reversed lists
// (head first).
void insert_or_append(const Clause& nc, std::vector<Clause>& rev_ors, std::vector<Clause>& rev_no) {
  Pat head = nc.p;
  const Pats& ps = nc.ps;
  auto safe_to_insert = [&](const std::vector<Clause>& rem, Pat p, const std::vector<Clause>& seen) {
    auto [_, not_e] = extract_equiv_head(p, rem);
    // check append condition for head of O
    if (!safe_below_or_matrix(not_e, p, ps)) return false;
    // check insert condition for tail of O
    for (auto& c : seen)
      if (!disjoint(p, c.p)) return false;
    return true;
  };
  std::vector<Clause> seen;  // head first
  for (std::size_t k = 0;; ++k) {
    // invariant: the new clause is safe to append at the end of [seen]
    if (k == rev_ors.size()) {
      rev_ors.insert(rev_ors.begin(), nc);
      return;
    }
    const Clause& cl = rev_ors[k];
    Pat p = head;
    Pat q = cl.p;
    if (!is_or(q) || disjoint(p, q)) {
      seen.insert(seen.begin(), cl);
      continue;
    }
    if (tt::pat_bound_idents(p).empty() && tt::pat_bound_idents(q).empty() && equiv_pat(p, q)) {
      // attempt insertion, for equivalent orpats with no variables
      std::vector<Clause> rem(rev_ors.begin() + static_cast<long>(k) + 1, rev_ors.end());
      if (safe_to_insert(rem, p, seen)) {
        // List.rev_append seen (new :: cl :: rem)
        std::vector<Clause> r(seen.rbegin(), seen.rend());
        r.push_back(nc);
        r.push_back(cl);
        r.insert(r.end(), rem.begin(), rem.end());
        rev_ors = std::move(r);
        return;
      }
      // fail to insert or append
      rev_no.insert(rev_no.begin(), nc);
      return;
    }
    if (safe_below(cl.ps, cl.act, ps)) {
      seen.insert(seen.begin(), cl);
      continue;
    }
    rev_no.insert(rev_no.begin(), nc);
    return;
  }
}
}  // namespace or_matrix

// Reconstruct default information from half_compiled pm list
Matrix as_matrix(const std::vector<Clause>& cases) {
  Matrix m;
  for (auto& c : cases) m.push_back(cons(c.p, c.ps));
  return get_mins(parmatch::le_pats, m);
}

// ---- splitting ----
std::pair<PmInfo, Nexts> split_or(const std::vector<Clause>& cls, const SplitArgs& args, const DefaultEnv& def);
std::pair<PmInfo, Nexts> split_no_or(const std::vector<Clause>& cls, const SplitArgs& args, const DefaultEnv& def,
                                     const Nexts& k);
std::pair<PmInfo, Nexts> precompile_var(const SplitArgs& args, const std::vector<Clause>& cls, const DefaultEnv& def,
                                        const Nexts& k);
std::pair<PmInfo, Nexts> do_not_precompile(const SplitArgs& args, const std::vector<Clause>& cls,
                                           const DefaultEnv& def, const Nexts& k);
std::pair<PmInfo, Nexts> precompile_or(const std::vector<Clause>& cls, const std::vector<Clause>& ors,
                                       const SplitArgs& args, const DefaultEnv& def, const Nexts& k);

std::pair<PmInfo, Nexts> split_or(const std::vector<Clause>& cls, const SplitArgs& args, const DefaultEnv& def) {
  std::function<std::pair<PmInfo, Nexts>(const std::vector<Clause>&)> do_split;
  auto cons_next = [&](const std::vector<Clause>& yes, const std::vector<Clause>& yesor,
                       const std::vector<Clause>& no) -> std::pair<PmInfo, Nexts> {
    DefaultEnv def2 = def;
    Nexts nexts;
    if (!no.empty()) {
      auto [info, nexts1] = do_split(no);
      long idef = L::next_raise_count();
      def2 = default_env::cons_(info.matrix, idef, info.top_default);
      nexts.push_back({idef, info.me});
      nexts.insert(nexts.end(), nexts1.begin(), nexts1.end());
    }
    if (yesor.empty()) return split_no_or(yes, args, def2, nexts);
    return precompile_or(yes, yesor, args, def2, nexts);
  };
  do_split = [&](const std::vector<Clause>& l) -> std::pair<PmInfo, Nexts> {
    std::vector<Clause> rev_before, rev_ors, rev_no;  // head first
    for (auto& cl : l) {
      if (!safe_before(cl, rev_no)) {
        rev_no.insert(rev_no.begin(), cl);
        continue;
      }
      if (is_simple_view(cl.p) && safe_before(cl, rev_ors)) {
        rev_before.insert(rev_before.begin(), cl);
        continue;
      }
      or_matrix::insert_or_append(cl, rev_ors, rev_no);
    }
    return cons_next(std::vector<Clause>(rev_before.rbegin(), rev_before.rend()),
                     std::vector<Clause>(rev_ors.rbegin(), rev_ors.rend()),
                     std::vector<Clause>(rev_no.rbegin(), rev_no.rend()));
  };
  return do_split(cls);
}

std::pair<PmInfo, Nexts> split_no_or(const std::vector<Clause>& cls, const SplitArgs& args, const DefaultEnv& def,
                                     const Nexts& k) {
  // We split the remaining clauses in as few pms as possible while
  // maintaining the property that for any pm in the result, it is possible
  // to decide for any two patterns on the first column whether their heads
  // are equal or not.
  std::function<std::pair<PmInfo, Nexts>(const std::vector<Clause>&)> split;
  auto should_split = [](HeadP group_discr) {
    // it is unlikely that we will raise anything, so we split now
    return group_discr->kind == HK::Construct &&
           group_discr->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension;
  };
  auto insert_split = [&](HeadP group_discr, const std::vector<Clause>& yes, const std::vector<Clause>& no,
                          const DefaultEnv& def, const Nexts& k) -> std::pair<PmInfo, Nexts> {
    auto precompile_group = [&](const SplitArgs& a, const std::vector<Clause>& c, const DefaultEnv& d,
                                const Nexts& kk) {
      if (group_discr->kind == HK::Any) return precompile_var(a, c, d, kk);
      return do_not_precompile(a, c, d, kk);
    };
    if (no.empty()) return precompile_group(args, yes, def, k);
    auto [info, nexts] = split(no);
    long idef = L::next_raise_count();
    Nexts nk{{idef, info.me}};
    nk.insert(nk.end(), nexts.begin(), nexts.end());
    return precompile_group(args, yes, default_env::cons_(info.matrix, idef, info.top_default), nk);
  };
  auto collect = [&](HeadP group_discr, const std::vector<Clause>& l) -> std::pair<PmInfo, Nexts> {
    std::vector<Clause> rev_yes, rev_no;  // head first
    for (std::size_t i = 0; i < l.size(); ++i) {
      const Clause& cl = l[i];
      if (i + 1 == l.size() && !rev_yes.empty() && simple_omega_like(cl.p)) {
        bool all = true;
        for (Pat p : cl.ps)
          if (!omega_like(p)) {
            all = false;
            break;
          }
        if (all) {
          // an extra division when the last row is made of variables only
          rev_no.insert(rev_no.begin(), cl);
          break;
        }
      }
      if (can_group(group_discr, cl.p) && safe_before(cl, rev_no)) {
        rev_yes.insert(rev_yes.begin(), cl);
        continue;
      }
      if (should_split(group_discr)) {
        if (!rev_no.empty()) fatal_error("Matching.split_no_or");
        std::vector<Clause> yes(rev_yes.rbegin(), rev_yes.rend());
        return insert_split(group_discr, yes, std::vector<Clause>(l.begin() + static_cast<long>(i), l.end()), def, k);
      }
      rev_no.insert(rev_no.begin(), cl);
    }
    return insert_split(group_discr, std::vector<Clause>(rev_yes.rbegin(), rev_yes.rend()),
                        std::vector<Clause>(rev_no.rbegin(), rev_no.rend()), def, k);
  };
  split = [&](const std::vector<Clause>& cls) {
    HeadP discr = what_is_first_case(cls);
    return collect(discr, cls);
  };
  return split(cls);
}

Matrix rebuild_matrix(const PmHalf& pmh) {
  switch (pmh.kind) {
    case PmHalf::K::Pm: return as_matrix(pmh.pm.cases);
    case PmHalf::K::PmOr: return pmh.or_matrix;
    case PmHalf::K::PmVar: return add_omega_column(rebuild_matrix(*pmh.inside));
  }
  return {};
}

std::pair<PmInfo, Nexts> precompile_var(const SplitArgs& args, const std::vector<Clause>& cls, const DefaultEnv& def,
                                        const Nexts& k) {
  // Strategy: pop the first column, precompile the rest, add a PmVar to all
  // precompiled submatrices.  If the rest doesn't generate any split, abort
  // and do_not_precompile.
  if (args.rest.empty()) return do_not_precompile(args, cls, def, k);
  const ArgT& first = args.rest[0];
  auto* lv = L::as<L::Lvar>(first.arg);
  if (!lv) return do_not_precompile(args, cls, def, k);
  Ident::t v = lv->id;
  // as split as it can
  if (cls.size() == 1) return do_not_precompile(args, cls, def, k);
  SplitArgs var_args{FirstArg{PureArg{v, nullptr}, first.binding_kind, first.mut},
                     Args(args.rest.begin() + 1, args.rest.end())};
  std::vector<Clause> var_cls;
  for (auto& c : cls) {
    if (!simple_omega_like(c.p)) fatal_error("Matching.precompile_var");
    var_cls.push_back(half_simplify_clause(L::lvar(v), InitialClause{c.ps, c.act}));
  }
  DefaultEnv var_def = default_env::pop_column(def);
  auto [info, nexts] = split_or(var_cls, var_args, var_def);
  if (nexts.empty()) return do_not_precompile(args, cls, def, k);
  // rebuild_default nexts def: fold_right from the end
  DefaultEnv top = def;
  for (auto it = nexts.rbegin(); it != nexts.rend(); ++it)
    top = default_env::cons_(add_omega_column(rebuild_matrix(*it->second)), it->first, top);
  auto pmvar = [](PmHalfP inside) {
    auto h = std::make_shared<PmHalf>();
    h->kind = PmHalf::K::PmVar;
    h->inside = std::move(inside);
    return PmHalfP(h);
  };
  PmInfo rfirst{pmvar(info.me), add_omega_column(info.matrix), top};
  Nexts rnexts;
  for (auto& [e, pm] : nexts) rnexts.push_back({e, pmvar(pm)});
  rnexts.insert(rnexts.end(), k.begin(), k.end());
  return {rfirst, rnexts};
}

std::pair<PmInfo, Nexts> do_not_precompile(const SplitArgs& args, const std::vector<Clause>& cls,
                                           const DefaultEnv& def, const Nexts& k) {
  auto h = std::make_shared<PmHalf>();
  h->kind = PmHalf::K::Pm;
  h->pm = SimplePM{cls, args, def};
  return {PmInfo{h, as_matrix(cls), def}, k};
}

std::pair<PmInfo, Nexts> precompile_or(const std::vector<Clause>& cls, const std::vector<Clause>& ors,
                                       const SplitArgs& args, const DefaultEnv& def, const Nexts& k) {
  std::vector<Clause> cases;
  std::vector<Handler> handlers;
  for (std::size_t i = 0; i < ors.size();) {
    const Clause& c = ors[i];
    if (is_simple_view(c.p)) {
      cases.push_back(c);
      ++i;
      continue;
    }
    // `Or _
    Pat orp = c.p;
    std::vector<Clause> rest(ors.begin() + static_cast<long>(i) + 1, ors.end());
    auto [others, rem] = extract_equiv_head(orp, rest);
    InitialPM orpm;
    orpm.def = default_env::pop_compat(orp, def);
    orpm.args = args.rest;
    orpm.cases.push_back({c.ps, c.act});
    for (auto& o : others) orpm.cases.push_back({o.ps, o.act});
    L::IdentSet pm_fv = pm_free_variables(orpm);
    // variables bound in the or-pattern that are used in the orpm actions
    std::vector<L::Param> patbound_action_vars;
    for (auto& b : tt::pat_bound_idents_full(orp))
      if (pm_fv.count(b.id)) patbound_action_vars.push_back({b.id, typeopt::value_kind(orp->pat_env, b.ty)});
    long or_num = L::next_raise_count();
    Pats new_patl = omega_list(c.ps);
    MkAction mk_new_action = [or_num](const std::vector<Ident::t>& vars) {
      std::vector<lam> args;
      for (Ident::t v : vars) args.push_back(L::lvar(v));
      return lstaticraise(or_num, args);
    };
    lam arg = arg_of_pure(args.first.arg);
    std::vector<Ident::t> vars;
    for (auto& pv : patbound_action_vars) vars.push_back(pv.id);
    for (auto& [p, act] : explode_or_pat(arg, c.p, mk_new_action, vars)) cases.push_back({p, new_patl, act});
    handlers.push_back(Handler{Matrix{Pats{orp}}, or_num, patbound_action_vars, std::move(orpm)});
    i += 1 + others.size();
  }
  std::vector<Clause> all = cls;
  all.insert(all.end(), ors.begin(), ors.end());
  Matrix matrix = as_matrix(all);
  std::vector<Clause> body_cases = cls;
  body_cases.insert(body_cases.end(), cases.begin(), cases.end());
  auto h = std::make_shared<PmHalf>();
  h->kind = PmHalf::K::PmOr;
  h->pm = SimplePM{body_cases, args, def};
  h->handlers = std::move(handlers);
  h->or_matrix = matrix;
  return {PmInfo{h, matrix, def}, k};
}

std::pair<PmHalfP, Nexts> split_and_precompile_simplified(const SimplePM& pm) {
  auto [info, nexts] = split_no_or(pm.cases, pm.args, pm.def, {});
  return {info.me, nexts};
}
std::pair<PmHalfP, Nexts> split_and_precompile_half_simplified(const SimplePM& pm) {
  auto [info, nexts] = split_or(pm.cases, pm.args, pm.def);
  return {info.me, nexts};
}

// ---- general divide functions ----
// a submatrix after specializing by discriminant pattern; [ctx] is the
// context shared by all rows.
struct Cell {
  InitialPM pm;
  Context ctx;
  HeadP discr;
};
using CellP = std::shared_ptr<Cell>;

// get_expr_args head first rest
using GetExprArgs = std::function<Args(HeadP, const ArgT&, const Args&)>;

CellP make_matching(const GetExprArgs& get_expr_args, HeadP head, const DefaultEnv& def0, const Context& ctx0,
                    const SplitArgs& sa) {
  DefaultEnv def = default_env::specialize(head, def0);
  ArgT first{arg_of_pure(sa.first.arg), sa.first.binding_kind, sa.first.mut};
  Args args = get_expr_args(head, first, sa.rest);
  Context ctx = context::specialize(head, ctx0);
  auto c = std::make_shared<Cell>();
  c->pm = InitialPM{{}, std::move(args), std::move(def)};
  c->ctx = std::move(ctx);
  c->discr = head;
  return c;
}

InitialPM make_line_matching(const GetExprArgs& get_expr_args, HeadP head, const DefaultEnv& def,
                             const SplitArgs& sa) {
  ArgT first{arg_of_pure(sa.first.arg), sa.first.binding_kind, sa.first.mut};
  // the record fields right to left: default, then args
  DefaultEnv d = default_env::specialize(head, def);
  Args args = get_expr_args(head, first, sa.rest);
  return InitialPM{{}, std::move(args), std::move(d)};
}

template <class Key>
struct Division {
  SplitArgs args;
  std::vector<std::pair<Key, CellP>> cells;  // head first
};

// Division built by a fold_right: add_in_div is called from the last case to
// the first; new cells and new rows are consed.  Here both are appended
// during the fold and the lists reversed at the end.
template <class Key>
struct DivBuilder {
  SplitArgs args;
  std::vector<std::pair<Key, CellP>> rev_cells;
  template <class Eq, class Mk>
  void add_in_div(Mk make_matching_fun, Eq eq_key, const Key& key, InitialClause patl_action) {
    for (auto& [k, cell] : rev_cells)
      if (eq_key(key, k)) {
        cell->pm.cases.push_back(std::move(patl_action));
        return;
      }
    CellP cell = make_matching_fun(args);
    cell->pm.cases.push_back(std::move(patl_action));
    rev_cells.push_back({key, cell});
  }
  Division<Key> finish() {
    Division<Key> d{args, {}};
    for (auto it = rev_cells.rbegin(); it != rev_cells.rend(); ++it) {
      std::reverse(it->second->pm.cases.begin(), it->second->pm.cases.end());
      d.cells.push_back(*it);
    }
    return d;
  }
};

template <class Key, class EqKey, class GetKey, class GetPatArgs>
Division<Key> divide(const GetExprArgs& get_expr_args, EqKey eq_key, GetKey get_key, GetPatArgs get_pat_args,
                     const Context& ctx, const SimplePM& pm) {
  DivBuilder<Key> b{pm.args, {}};
  for (auto it = pm.cases.rbegin(); it != pm.cases.rend(); ++it) {
    HeadP ph = simple_head(it->p);
    Pat p = it->p;
    InitialClause pa{get_pat_args(p, it->ps), it->act};
    Key key = get_key(p);
    b.add_in_div([&](const SplitArgs& a) { return make_matching(get_expr_args, ph, pm.def, ctx, a); }, eq_key, key,
                 std::move(pa));
  }
  return b.finish();
}

template <class MakeCtx, class GetPatArgs>
Cell divide_line(MakeCtx make_ctx, const GetExprArgs& get_expr_args, GetPatArgs get_pat_args, HeadP discr,
                 const Context& ctx, const SimplePM& pm) {
  InitialPM line = make_line_matching(get_expr_args, discr, pm.def, pm.args);
  std::vector<InitialClause> rev;
  for (auto it = pm.cases.rbegin(); it != pm.cases.rend(); ++it) rev.push_back({get_pat_args(it->p, it->ps), it->act});
  line.cases.assign(rev.rbegin(), rev.rend());
  Context c = make_ctx(ctx);
  return Cell{std::move(line), std::move(c), discr};
}

Pats drop_pat_arg(Pat, const Pats& rem) { return rem; }
Args drop_expr_arg(HeadP, const ArgT&, const Args& rem) { return rem; }
Args cons_arg(ArgT a, const Args& rem) {
  Args r{std::move(a)};
  r.insert(r.end(), rem.begin(), rem.end());
  return r;
}

// Matching against a constant
tt::Constant get_key_constant(Pat p) {
  if (auto* c = as<tt::Tpat_constant>(p->pat_desc)) return c->c;
  fatal_error("BAD(divide)");
}
Division<tt::Constant> divide_constant(const Context& ctx, const SimplePM& m) {
  return divide<tt::Constant>(
      drop_expr_arg, [](const tt::Constant& c, const tt::Constant& d) { return parmatch::const_compare(c, d) == 0; },
      get_key_constant, drop_pat_arg, ctx, m);
}

// Matching against a constructor
const ConstructorDescription* get_key_constr(Pat p) {
  if (auto* c = as<tt::Tpat_construct>(p->pat_desc)) return c->cstr;
  fatal_error("Matching.get_key_constr");
}
Pats get_pat_args_constr(Pat p, const Pats& rem) {
  auto* c = as<tt::Tpat_construct>(p->pat_desc);
  if (!c) fatal_error("Matching.get_pat_args_constr");
  return append(Pats(c->args.begin(), c->args.end()), rem);
}

Args get_expr_args_constr(scopes sc, HeadP head, const ArgT& a, const Args& rem) {
  if (head->kind != HK::Construct) fatal_error("Matching.get_expr_args_constr");
  const ConstructorDescription* cstr = head->cstr;
  L::ScopedLocation loc = head_loc(sc, head);
  auto make_field_accesses = [&](LetKind binding_kind, long first_pos, long last_pos, const Args& argl) {
    Args r;
    for (long pos = first_pos; pos <= last_pos; ++pos)
      r.push_back(ArgT{lprim(pfield(pos, L::ImmediateOrPointer::Pointer, MutableFlag::Immutable), {a.arg}, loc),
                       binding_kind, compose_mut(a.mut, MutableFlag::Immutable)});
    r.insert(r.end(), argl.begin(), argl.end());
    return r;
  };
  if (cstr->cstr_inlined) return cons_arg(ArgT{a.arg, LetKind::Alias, a.mut}, rem);
  switch (cstr->cstr_tag.kind) {
    case ConstructorTag::Kind::Cstr_constant:
    case ConstructorTag::Kind::Cstr_block: return make_field_accesses(LetKind::Alias, 0, cstr->cstr_arity - 1, rem);
    case ConstructorTag::Kind::Cstr_unboxed: return cons_arg(ArgT{a.arg, LetKind::Alias, a.mut}, rem);
    case ConstructorTag::Kind::Cstr_extension: return make_field_accesses(LetKind::Alias, 1, cstr->cstr_arity, rem);
  }
  fatal_error("Matching.get_expr_args_constr");
}

Division<const ConstructorDescription*> divide_constructor(scopes sc, const Context& ctx, const SimplePM& pm) {
  return divide<const ConstructorDescription*>(
      [sc](HeadP h, const ArgT& a, const Args& r) { return get_expr_args_constr(sc, h, a, r); },
      data_types::equal_constr, get_key_constr, get_pat_args_constr, ctx, pm);
}

// Matching against a variant
Args get_expr_args_variant_nonconst(scopes sc, HeadP head, const ArgT& a, const Args& rem) {
  L::ScopedLocation loc = head_loc(sc, head);
  return cons_arg(ArgT{lprim(pfield(1, L::ImmediateOrPointer::Pointer, MutableFlag::Immutable), {a.arg}, loc),
                       LetKind::Alias, compose_mut(a.mut, MutableFlag::Immutable)},
                  rem);
}

// (Cstr_constant tag | Cstr_block tag) compared with (=)
struct VariantKey {
  bool block;
  long tag;
};
bool operator==(const VariantKey& a, const VariantKey& b) { return a.block == b.block && a.tag == b.tag; }

Division<VariantKey> divide_variant(scopes sc, const RowDesc* row, const Context& ctx, const SimplePM& pm) {
  DivBuilder<VariantKey> b{pm.args, {}};
  auto eq = [](const VariantKey& a, const VariantKey& b) { return a == b; };
  for (auto it = pm.cases.rbegin(); it != pm.cases.rend(); ++it) {
    auto* v = as<tt::Tpat_variant>(it->p->pat_desc);
    if (!v) fatal_error("Matching.divide_variant");
    std::string_view lab = v->label;
    HeadP head = simple_head(it->p);
    if (row_field_repr(get_row_field(lab, row)).kind == RowFieldView::Kind::Rabsent) continue;
    long tag = btype::hash_variant(lab);
    if (!v->arg) {
      b.add_in_div([&](const SplitArgs& a) { return make_matching(drop_expr_arg, head, pm.def, ctx, a); }, eq,
                   VariantKey{false, tag}, InitialClause{it->ps, it->act});
    } else {
      b.add_in_div(
          [&](const SplitArgs& a) {
            return make_matching(
                [sc](HeadP h, const ArgT& x, const Args& r) { return get_expr_args_variant_nonconst(sc, h, x, r); },
                head, pm.def, ctx, a);
          },
          eq, VariantKey{true, tag}, InitialClause{cons(v->arg, it->ps), it->act});
    }
  }
  return b.finish();
}

// Matching against a variable
Cell divide_var(const Context& ctx, const SimplePM& pm) {
  return divide_line(context::lshift, drop_expr_arg, drop_pat_arg, head_omega(), ctx, pm);
}

// Matching and forcing a lazy value
Pats get_pat_args_lazy(Pat p, const Pats& rem) {
  if (p->pat_desc->kind == PK::Tpat_any) return cons(omega(), rem);
  if (auto* l = as<tt::Tpat_lazy>(p->pat_desc)) return cons(l->pat, rem);
  fatal_error("Matching.get_pat_args_lazy");
}

const PrimitiveDescription* prim_obj_tag() {
  static const PrimitiveDescription* d = primitive_simple("caml_obj_tag", 1, false);
  return d;
}

// lazy (transl_prim "CamlinternalLazy" ...): forced once per process
lam code_force_lazy_block() {
  static lam l = [] {
    ZoneScope perm(permanent_zone());
    return L::transl_prim("CamlinternalLazy", "force_lazy_block");
  }();
  return l;
}
lam code_force_lazy() {
  static lam l = [] {
    ZoneScope perm(permanent_zone());
    return L::transl_prim("CamlinternalLazy", "force_gen");
  }();
  return l;
}

lam call_force_lazy_block(lam varg, const L::ScopedLocation& loc) {
  // The argument is wrapped with [Popaque] to prevent the rest of the
  // compiler from making any assumptions on its contents.
  lam force_fun = code_force_lazy_block();
  L::LambdaApply ap;
  ap.ap_tailcall = L::TailcallAttribute::Default_tailcall;
  ap.ap_loc = loc;
  ap.ap_func = force_fun;
  ap.ap_args = slice(std::vector<lam>{lprim(L::prim(PrimK::Popaque), {varg}, loc)});
  ap.ap_inlined = L::InlineAttribute{};
  ap.ap_specialised = L::SpecialiseAttribute::Default_specialise;
  return L::lapply(ap);
}

lam inline_lazy_force_cond(lam arg, const L::ScopedLocation& loc) {
  Ident::t idarg = Ident::create_local("lzarg");
  lam varg = L::lvar(idarg);
  Ident::t tag = Ident::create_local("tag");
  auto test_tag = [&](long t) {
    return lprim(pintcomp(L::IntegerComparison::Ceq), {L::lvar(tag), lconst_int(t)}, loc);
  };
  // the constructor arguments right to left
  lam forced = call_force_lazy_block(varg, loc);
  lam seqor = lprim(L::prim(PrimK::Psequor), {test_tag(lazy_tag), test_tag(forcing_tag)}, loc);
  lam inner = L::lifthenelse(seqor, forced, varg);
  lam field0 = lprim(pfield(0, L::ImmediateOrPointer::Pointer, MutableFlag::Mutable), {varg}, loc);
  lam ite = L::lifthenelse(test_tag(forward_tag), field0, inner);
  return L::llet(LetKind::Strict, L::ValueKind::gen(), idarg, arg,
                 L::llet(LetKind::Alias, L::ValueKind::gen(), tag, lprim(pccall(prim_obj_tag()), {varg}, loc), ite));
}

lam inline_lazy_force_switch(lam arg, const L::ScopedLocation& loc) {
  Ident::t idarg = Ident::create_local("lzarg");
  lam varg = L::lvar(idarg);
  L::LambdaSwitch sw;
  sw.sw_numblocks = 0;
  sw.sw_blocks = {};
  sw.sw_numconsts = 256;  // PR#6033 - tag ranges from 0 to 255
  sw.sw_failaction = varg;
  lam f2 = call_force_lazy_block(varg, loc);
  lam f1 = call_force_lazy_block(varg, loc);
  sw.sw_consts = slice(std::vector<L::SwitchCase>{
      {forward_tag, lprim(pfield(0, L::ImmediateOrPointer::Pointer, MutableFlag::Mutable), {varg}, loc)},
      {lazy_tag, f1},
      {forcing_tag, f2}});
  return L::llet(LetKind::Strict, L::ValueKind::gen(), idarg, arg,
                 L::lifthenelse(lprim(L::prim(PrimK::Pisint), {varg}, loc), varg,
                                L::lswitch(lprim(pccall(prim_obj_tag()), {varg}, loc), sw, loc)));
}

lam inline_lazy_force_(lam arg, const L::ScopedLocation& loc) {
  if (afl_instrument) {
    // Disable inlining optimisation if AFL instrumentation active
    L::LambdaApply ap;
    ap.ap_tailcall = L::TailcallAttribute::Default_tailcall;
    ap.ap_loc = loc;
    ap.ap_func = code_force_lazy();
    ap.ap_args = slice(std::vector<lam>{lconst_int(0), arg});
    ap.ap_inlined = L::InlineAttribute{L::InlineAttribute::Kind::Never_inline, 0};
    ap.ap_specialised = L::SpecialiseAttribute::Default_specialise;
    return L::lapply(ap);
  }
  if (clflags::native_code)
    // Lswitch generates compact and efficient native code
    return inline_lazy_force_switch(arg, loc);
  // generating bytecode: conditionals are better
  return inline_lazy_force_cond(arg, loc);
}

Args get_expr_args_lazy(scopes sc, HeadP head, const ArgT& a, const Args& rem) {
  L::ScopedLocation loc = head_loc(sc, head);
  // A lazy pattern is considered immutable
  return cons_arg(ArgT{inline_lazy_force_(a.arg, loc), LetKind::Strict, compose_mut(a.mut, MutableFlag::Immutable)},
                  rem);
}

Cell divide_lazy(scopes sc, HeadP head, const Context& ctx, const SimplePM& pm) {
  return divide_line([head](const Context& c) { return context::specialize(head, c); },
                     [sc](HeadP h, const ArgT& a, const Args& r) { return get_expr_args_lazy(sc, h, a, r); },
                     get_pat_args_lazy, head, ctx, pm);
}

// Matching against a tuple pattern
Pats get_pat_args_tuple(long arity, Pat p, const Pats& rem) {
  if (p->pat_desc->kind == PK::Tpat_any) return append(omegas(arity), rem);
  if (auto* t = as<tt::Tpat_tuple>(p->pat_desc)) {
    Pats ps;
    for (auto& x : t->pats) ps.push_back(x.pat);
    return append(ps, rem);
  }
  fatal_error("Matching.get_pat_args_tuple");
}

Args get_expr_args_tuple(scopes sc, HeadP head, const ArgT& a, const Args& rem) {
  L::ScopedLocation loc = head_loc(sc, head);
  long arity = head_arity(head);
  Args r;
  for (long pos = 0; pos < arity; ++pos)
    r.push_back(ArgT{lprim(pfield(pos, L::ImmediateOrPointer::Pointer, MutableFlag::Immutable), {a.arg}, loc),
                     LetKind::Alias, compose_mut(a.mut, MutableFlag::Immutable)});
  r.insert(r.end(), rem.begin(), rem.end());
  return r;
}

Cell divide_tuple(scopes sc, HeadP head, const Context& ctx, const SimplePM& pm) {
  long arity = head_arity(head);
  return divide_line([head](const Context& c) { return context::specialize(head, c); },
                     [sc](HeadP h, const ArgT& a, const Args& r) { return get_expr_args_tuple(sc, h, a, r); },
                     [arity](Pat p, const Pats& rem) { return get_pat_args_tuple(arity, p, rem); }, head, ctx, pm);
}

// Matching against a record pattern
Pats record_matching_line(long num_fields, Slice<tt::RecordPatField> lbl_pat_list) {
  Pats patv(static_cast<std::size_t>(num_fields), omega());
  for (auto& f : lbl_pat_list) patv[static_cast<std::size_t>(f.label->lbl_pos)] = f.pat;
  return patv;
}
Pats get_pat_args_record(long num_fields, Pat p, const Pats& rem) {
  if (p->pat_desc->kind == PK::Tpat_any) return append(record_matching_line(num_fields, {}), rem);
  if (auto* r = as<tt::Tpat_record>(p->pat_desc)) return append(record_matching_line(num_fields, r->fields), rem);
  fatal_error("Matching.get_pat_args_record");
}

Args get_expr_args_record(scopes sc, HeadP head, const ArgT& a, const Args& rem) {
  L::ScopedLocation loc = head_loc(sc, head);
  if (head->kind != HK::Record || head->lbls.empty()) fatal_error("Matching.get_expr_args_record");
  auto all_labels = head->lbls[0]->lbl_all;
  Args r;
  for (std::size_t pos = 0; pos < all_labels.size(); ++pos) {
    const LabelDescription* lbl = all_labels[pos];
    L::ImmediateOrPointer ptr = typeopt::maybe_pointer_type(head->pat_env, lbl->lbl_arg);
    lam access;
    switch (lbl->lbl_repres.kind) {
      case RecordRepresentation::Kind::Record_regular:
      case RecordRepresentation::Kind::Record_inlined:
        access = lprim(pfield(lbl->lbl_pos, ptr, lbl->lbl_mut), {a.arg}, loc);
        break;
      case RecordRepresentation::Kind::Record_unboxed: access = a.arg; break;
      case RecordRepresentation::Kind::Record_float: access = lprim(pfloatfield(lbl->lbl_pos), {a.arg}, loc); break;
      case RecordRepresentation::Kind::Record_extension:
        access = lprim(pfield(lbl->lbl_pos + 1, ptr, lbl->lbl_mut), {a.arg}, loc);
        break;
    }
    LetKind binding_kind = lbl->lbl_mut == MutableFlag::Immutable ? LetKind::Alias : LetKind::StrictOpt;
    r.push_back(ArgT{access, binding_kind, compose_mut(a.mut, lbl->lbl_mut)});
  }
  r.insert(r.end(), rem.begin(), rem.end());
  return r;
}

Cell divide_record(Slice<const LabelDescription*> all_labels, scopes sc, HeadP head0, const Context& ctx,
                   const SimplePM& pm) {
  HeadP head = expand_record_head(head0);
  long n = static_cast<long>(all_labels.size());
  return divide_line([head](const Context& c) { return context::specialize(head, c); },
                     [sc](HeadP h, const ArgT& a, const Args& r) { return get_expr_args_record(sc, h, a, r); },
                     [n](Pat p, const Pats& rem) { return get_pat_args_record(n, p, rem); }, head, ctx, pm);
}

// Matching against an array pattern
long get_key_array(Pat p) {
  if (auto* a = as<tt::Tpat_array>(p->pat_desc)) return static_cast<long>(a->pats.size());
  fatal_error("Matching.get_key_array");
}
Pats get_pat_args_array(Pat p, const Pats& rem) {
  if (auto* a = as<tt::Tpat_array>(p->pat_desc)) return append(Pats(a->pats.begin(), a->pats.end()), rem);
  fatal_error("Matching.get_pat_args_array");
}
Args get_expr_args_array(scopes sc, L::ArrayKind kind, HeadP head, const ArgT& a, const Args& rem) {
  if (head->kind != HK::Array) fatal_error("Matching.get_expr_args_array");
  MutableFlag am = head->am;
  long len = head->n;
  L::ScopedLocation loc = head_loc(sc, head);
  Args r;
  for (long pos = 0; pos < len; ++pos)
    r.push_back(ArgT{lprim(parray(PrimK::Parrayrefu, kind), {a.arg, lconst_int(pos)}, loc),
                     am == MutableFlag::Mutable ? LetKind::StrictOpt : LetKind::Alias, compose_mut(a.mut, am)});
  r.insert(r.end(), rem.begin(), rem.end());
  return r;
}
Division<long> divide_array(scopes sc, L::ArrayKind kind, const Context& ctx, const SimplePM& pm) {
  return divide<long>(
      [sc, kind](HeadP h, const ArgT& a, const Args& r) { return get_expr_args_array(sc, kind, h, a, r); },
      [](long a, long b) { return a == b; }, get_key_array, get_pat_args_array, ctx, pm);
}

// ---- specific string test sequence ----
constexpr long strings_test_threshold = 8;

const PrimitiveDescription* prim_string_notequal_desc() {
  static const PrimitiveDescription* d = primitive_simple("caml_string_notequal", 2, false);
  return d;
}
const PrimitiveDescription* prim_string_compare_desc() {
  static const PrimitiveDescription* d = primitive_simple("caml_string_compare", 2, false);
  return d;
}

lam bind_sw(lam arg, const std::function<lam(lam)>& k) {
  if (L::as<L::Lvar>(arg)) return k(arg);
  Ident::t id = Ident::create_local("switch");
  return L::llet(LetKind::Strict, L::ValueKind::gen(), id, arg, k(L::lvar(id)));
}

using StrCases = std::vector<L::StringCase>;

// Sequential equality tests
lam make_string_test_sequence(const L::ScopedLocation& loc, lam arg, StrCases sw, lam d) {
  if (!d) {
    if (sw.empty()) fatal_error("Matching.make_string_test_sequence");
    d = sw[0].action;
    sw.erase(sw.begin());
  }
  return bind_sw(arg, [&](lam arg) {
    lam k = d;
    for (auto it = sw.rbegin(); it != sw.rend(); ++it)
      k = L::lifthenelse(lprim(pccall(prim_string_notequal_desc()), {arg, lconst_immstring(it->s)}, loc), k,
                         it->action);
    return k;
  });
}

lam zero_lam() { return lconst_int(0); }

lam tree_way_test(const L::ScopedLocation& loc, lam arg, lam lt, lam eq, lam gt) {
  return L::lifthenelse(lprim(pintcomp(L::IntegerComparison::Clt), {arg, zero_lam()}, loc), lt,
                        L::lifthenelse(lprim(pintcomp(L::IntegerComparison::Clt), {zero_lam(), arg}, loc), gt, eq));
}

// Dichotomic tree
lam do_make_string_test_tree(const L::ScopedLocation& loc, lam arg, const StrCases& sw, long delta, lam d) {
  long len = static_cast<long>(sw.size());
  if (len <= strings_test_threshold + delta) return make_string_test_sequence(loc, arg, sw, d);
  // split len sw
  long k = len;
  std::size_t m = 0;
  while (k > 1) {
    k -= 2;
    ++m;
  }
  StrCases lt(sw.begin(), sw.begin() + static_cast<long>(m));
  const L::StringCase& mid = sw[m];
  StrCases gt(sw.begin() + static_cast<long>(m) + 1, sw.end());
  return bind_sw(lprim(pccall(prim_string_compare_desc()), {arg, lconst_immstring(mid.s)}, loc), [&](lam r) {
    // tree_way_test's arguments right to left
    lam g = do_make_string_test_tree(loc, arg, gt, delta, d);
    lam l = do_make_string_test_tree(loc, arg, lt, delta, d);
    return tree_way_test(loc, r, l, mid.action, g);
  });
}

// ---- generic test trees ----
// Add handler, if shared
struct HandleShared {
  std::function<lam(lam)> hs = [](lam x) { return x; };
  lam operator()(const switch_::Shared<lam>& act) {
    if (!act.shared) return act.act;
    auto [i, h] = make_catch_delayed(act.act);
    auto ohs = hs;
    hs = [h, ohs](lam a) { return h(ohs(a)); };
    return make_exit(i);
  }
};

template <class C>
struct Shared3 {
  std::function<lam(lam)> hs;
  std::vector<std::pair<C, lam>> sw;
  lam d;
};
template <class C>
Shared3<C> share_actions_tree(const std::vector<std::pair<C, lam>>& sw, lam d) {
  StoreExp store;
  // Default action is always shared
  std::optional<long> di;
  if (d) di = store.act_store_shared(d);
  // Store all other actions
  std::vector<std::pair<C, long>> swi;
  for (auto& [cst, act] : sw) swi.push_back({cst, store.act_store(act)});
  // Retrieve all actions, including potential default
  auto shared = store.act_get_shared();
  HandleShared handle_shared;
  std::vector<lam> acts;
  for (auto& a : shared) acts.push_back(handle_shared(a));
  Shared3<C> r;
  r.d = di ? acts[static_cast<std::size_t>(*di)] : nullptr;
  for (auto& [cst, j] : swi) r.sw.push_back({cst, acts[static_cast<std::size_t>(j)]});
  r.hs = handle_shared.hs;
  return r;
}

using ConstCases = std::vector<std::pair<tt::Constant, lam>>;

// Note: dichotomic search requires sorted input with no duplicates
ConstCases uniq_lambda_list(const ConstCases& sw) {
  ConstCases r;
  for (auto& x : sw) {
    if (!r.empty() && parmatch::const_compare(r.back().first, x.first) == 0) continue;
    r.push_back(x);
  }
  return r;
}
ConstCases sort_lambda_list(const ConstCases& l) {
  auto sorted = ocaml_list::stable_sort(
      [](const std::pair<tt::Constant, lam>& x, const std::pair<tt::Constant, lam>& y) {
        return parmatch::const_compare(x.first, y.first);
      },
      l);
  return uniq_lambda_list(sorted);
}

lam do_tests_fail(const L::ScopedLocation& loc, lam fail, const L::Primitive& tst, lam arg, const ConstCases& l,
                  std::size_t from, std::size_t to) {
  lam r = fail;
  for (std::size_t k = to; k-- > from;)
    r = L::lifthenelse(lprim(tst, {arg, L::lambda_of_const(l[k].first)}, loc), r, l[k].second);
  return r;
}
lam do_tests_nofail(const L::ScopedLocation& loc, const L::Primitive& tst, lam arg, const ConstCases& l,
                    std::size_t from, std::size_t to) {
  if (from == to) fatal_error("Matching.do_tests_nofail");
  lam r = l[to - 1].second;
  for (std::size_t k = to - 1; k-- > from;)
    r = L::lifthenelse(lprim(tst, {arg, L::lambda_of_const(l[k].first)}, loc), r, l[k].second);
  return r;
}

lam make_test_sequence(const L::ScopedLocation& loc, lam fail, const L::Primitive& tst, const L::Primitive& lt_tst,
                       lam arg, const ConstCases& const_lambda_list0) {
  ConstCases sorted = sort_lambda_list(const_lambda_list0);
  Shared3<tt::Constant> s = share_actions_tree(sorted, fail);
  const ConstCases& l = s.sw;
  lam f = s.d;
  std::function<lam(std::size_t, std::size_t)> mts = [&](std::size_t from, std::size_t to) -> lam {
    std::size_t n = to - from;
    if (n >= 4 && lt_tst.kind != PrimK::Pignore) {
      // split_sequence
      std::size_t mid = from + n / 2;
      // Lifthenelse (.., make_test_sequence list1, make_test_sequence list2):
      // right to left
      lam l2 = mts(mid, to);
      lam l1 = mts(from, mid);
      return L::lifthenelse(lprim(lt_tst, {arg, L::lambda_of_const(l[mid].first)}, loc), l1, l2);
    }
    if (!f) return do_tests_nofail(loc, tst, arg, l, from, to);
    return do_tests_fail(loc, f, tst, arg, l, from, to);
  };
  return s.hs(mts(0, l.size()));
}

// ---- SArg ----
struct SArg {
  using primitive = L::Primitive;
  using loc = L::ScopedLocation;
  using arg = lam;
  using test = lam;
  using act = lam;

  static primitive eqint() { return pintcomp(L::IntegerComparison::Ceq); }
  static primitive neint() { return pintcomp(L::IntegerComparison::Cne); }
  static primitive leint() { return pintcomp(L::IntegerComparison::Cle); }
  static primitive ltint() { return pintcomp(L::IntegerComparison::Clt); }
  static primitive geint() { return pintcomp(L::IntegerComparison::Cge); }
  static primitive gtint() { return pintcomp(L::IntegerComparison::Cgt); }

  static test make_prim(const primitive& p, std::vector<arg> args) { return lprim(p, std::move(args), loc_unknown()); }
  static arg make_offset(arg a, long n) {
    if (n == 0) return a;
    return lprim(poffsetint(n), {a}, loc_unknown());
  }
  static act bind(arg a, const std::function<act(arg)>& body) {
    Ident::t newvar;
    lam newarg;
    if (auto* v = L::as<L::Lvar>(a)) {
      newvar = v->id;
      newarg = a;
    } else {
      newvar = Ident::create_local("switcher");
      newarg = L::lvar(newvar);
    }
    return L::bind(LetKind::Alias, newvar, a, body(newarg));
  }
  static arg make_const(long i) { return lconst_int(i); }
  static test make_isout(arg h, arg a) { return lprim(L::prim(PrimK::Pisout), {h, a}, loc_unknown()); }
  static test make_isin(arg h, arg a) { return lprim(L::prim(PrimK::Pnot), {make_isout(h, a)}, loc_unknown()); }
  static test make_is_nonzero(arg a) {
    if (clflags::native_code) return lprim(pintcomp(L::IntegerComparison::Cne), {a, lconst_int(0)}, loc_unknown());
    return a;
  }
  static test arg_as_test(arg a) { return a; }
  static act make_if(test cond, act ifso, act ifnot) { return L::lifthenelse(cond, ifso, ifnot); }
  static act make_switch(const loc& lc, arg a, const std::vector<long>& cases, std::vector<act>& acts) {
    // If several entries in the [cases] array point to the same action, we
    // must share it to avoid duplicating terms (PR#11893).
    std::vector<long> act_uses(acts.size(), 0);
    for (long c : cases) act_uses[static_cast<std::size_t>(c)] += 1;
    std::function<lam(lam)> wrapper = [](lam l) { return l; };
    for (std::size_t j = 0; j < acts.size(); ++j)
      if (act_uses[j] > 1) {
        auto [nfail, wrap] = make_catch_delayed(acts[j]);
        acts[j] = make_exit(nfail);
        auto prev_wrapper = wrapper;
        wrapper = [wrap, prev_wrapper](lam l) { return wrap(prev_wrapper(l)); };
      }
    std::vector<L::SwitchCase> l;
    for (std::size_t i = 0; i < cases.size(); ++i)
      l.push_back({static_cast<long>(i), acts[static_cast<std::size_t>(cases[i])]});
    L::LambdaSwitch sw;
    sw.sw_numconsts = static_cast<long>(cases.size());
    sw.sw_consts = slice(l);
    sw.sw_numblocks = 0;
    sw.sw_blocks = {};
    sw.sw_failaction = nullptr;
    return wrapper(L::lswitch(a, sw, lc));
  }
  static std::pair<long, std::function<act(act)>> make_catch(act a) { return make_catch_delayed(a); }
  static act make_exit(long i) { return matching::make_exit(i); }
};

using Switcher = switch_::Make<SArg>;

// Action sharing for Lswitch argument
std::pair<std::function<lam(lam)>, L::LambdaSwitch> share_actions_sw(const L::LambdaSwitch& sw) {
  // Attempt sharing on all actions
  StoreExp store;
  // Fail is translated to exit, whatever happens
  std::optional<long> fail;
  if (sw.sw_failaction) fail = store.act_store_shared(sw.sw_failaction);
  std::vector<std::pair<long, long>> consts, blocks;
  for (auto& c : sw.sw_consts) consts.push_back({c.key, store.act_store(c.action)});
  for (auto& c : sw.sw_blocks) blocks.push_back({c.key, store.act_store(c.action)});
  auto shared = store.act_get_shared();
  HandleShared handle_shared;
  std::vector<lam> acts;
  for (auto& a : shared) acts.push_back(handle_shared(a));
  L::LambdaSwitch r = sw;
  std::vector<L::SwitchCase> c2, b2;
  for (auto& [i, j] : consts) c2.push_back({i, acts[static_cast<std::size_t>(j)]});
  for (auto& [i, j] : blocks) b2.push_back({i, acts[static_cast<std::size_t>(j)]});
  r.sw_consts = slice(c2);
  r.sw_blocks = slice(b2);
  r.sw_failaction = fail ? acts[static_cast<std::size_t>(*fail)] : nullptr;
  return {handle_shared.hs, r};
}

// Reintroduce fail action in switch argument, for the sake of avoiding
// carrying over huge switches
L::LambdaSwitch reintroduce_fail(const L::LambdaSwitch& sw) {
  if (sw.sw_failaction) return sw;
  std::map<long, long> t;
  auto seen = [&](const L::SwitchCase& c) {
    if (auto i = as_simple_exit(c.action)) t[*i] += 1;
  };
  for (auto& c : sw.sw_consts) seen(c);
  for (auto& c : sw.sw_blocks) seen(c);
  long c_max = -1;
  long i_max = switch_::ocaml_max_int;
  for (auto& [i, c] : t) {
    if (c > c_max) {
      i_max = i;
      c_max = c;
    } else if (c == c_max) {
      // Pick the minimal [i] which has maximal [c]
      i_max = std::min(i, i_max);
    }
  }
  if (c_max >= 3) {
    long def = i_max;
    auto remove = [&](Slice<L::SwitchCase> l) {
      std::vector<L::SwitchCase> r;
      for (auto& c : l) {
        auto j = as_simple_exit(c.action);
        if (!j || *j != def) r.push_back(c);
      }
      return slice(r);
    };
    L::LambdaSwitch r = sw;
    r.sw_consts = remove(sw.sw_consts);
    r.sw_blocks = remove(sw.sw_blocks);
    r.sw_failaction = make_exit(def);
    return r;
  }
  return sw;
}

using IntCases = std::vector<std::pair<long, lam>>;
using SwCases = Switcher::Cases;

std::pair<long, long> get_edges(long low, long high, const IntCases& l) {
  if (l.empty()) return {low, high};
  return {l.front().first, l.back().first};
}

std::pair<SwCases, std::shared_ptr<StoreExp>> as_interval_canfail(lam fail, long low, long high, const IntCases& l) {
  using switch_::iadd;
  using switch_::isub;
  auto store = std::make_shared<StoreExp>();
  auto do_store = [&](lam act) { return store->act_store(act); };
  // fail has action index 0
  if (do_store(fail) != 0) fatal_error("Matching.as_interval_canfail");
  SwCases r;
  // the mutually recursive nofail_rec / fail_rec / init_rec, as a loop
  enum class St { Nofail, Fail } st;
  long cur_low, cur_high, cur_act = 0;
  std::size_t k = 0;
  if (l.empty()) {
    r.push_back({low, high, 0});
    return {r, store};
  }
  {
    auto [i, act_i] = l[0];
    long index = do_store(act_i);
    k = 1;
    if (index == 0) {
      st = St::Fail;
      cur_low = low;
      cur_high = i;
    } else {
      if (low < i) r.push_back({low, isub(i, 1), 0});
      st = St::Nofail;
      cur_low = i;
      cur_high = i;
      cur_act = index;
    }
  }
  for (;;) {
    if (st == St::Nofail) {
      if (k == l.size()) {
        if (cur_high == high) r.push_back({cur_low, cur_high, cur_act});
        else {
          r.push_back({cur_low, cur_high, cur_act});
          r.push_back({iadd(cur_high, 1), high, 0});
        }
        break;
      }
      auto [i, act_i] = l[k];
      long act_index = do_store(act_i);
      if (iadd(cur_high, 1) == i) {
        if (act_index == cur_act) {
          cur_high = i;
          ++k;
        } else if (act_index == 0) {
          r.push_back({cur_low, isub(i, 1), cur_act});
          st = St::Fail;
          cur_low = i;
          cur_high = i;
          ++k;
        } else {
          r.push_back({cur_low, isub(i, 1), cur_act});
          cur_low = i;
          cur_high = i;
          cur_act = act_index;
          ++k;
        }
      } else if (act_index == 0) {
        r.push_back({cur_low, cur_high, cur_act});
        // fail_rec (cur_high + 1) (cur_high + 1) all: [all] again
        st = St::Fail;
        cur_low = iadd(cur_high, 1);
        cur_high = cur_low;
      } else {
        r.push_back({cur_low, cur_high, cur_act});
        r.push_back({iadd(cur_high, 1), isub(i, 1), 0});
        cur_low = i;
        cur_high = i;
        cur_act = act_index;
        ++k;
      }
    } else {
      if (k == l.size()) {
        r.push_back({cur_low, cur_high, 0});
        break;
      }
      auto [i, act_i] = l[k];
      long index = do_store(act_i);
      ++k;
      if (index == 0) {
        cur_high = i;
      } else {
        r.push_back({cur_low, isub(i, 1), 0});
        st = St::Nofail;
        cur_low = i;
        cur_high = i;
        cur_act = index;
      }
    }
  }
  return {r, store};
}

std::pair<SwCases, std::shared_ptr<StoreExp>> as_interval_nofail(const IntCases& l) {
  auto store = std::make_shared<StoreExp>();
  if (l.empty()) fatal_error("Matching.as_interval_nofail");
  auto some_hole = [&](std::size_t from) {
    for (std::size_t k = from; k + 1 < l.size(); ++k)
      if (l[k + 1].first > switch_::iadd(l[k].first, 1)) return true;
    return false;
  };
  SwCases inters;
  auto [i0, act0] = l[0];
  // In case there is some hole and that a switch is emitted, action 0 will
  // be used as the action of unreachable cases.  Hence, this action will be
  // shared
  long act_index = some_hole(1) ? store->act_store_shared(act0) : store->act_store(act0);
  if (act_index != 0) fatal_error("Matching.as_interval_nofail");
  long cur_low = i0, cur_high = i0, cur_act = act_index;
  for (std::size_t k = 1; k < l.size(); ++k) {
    auto [i, act] = l[k];
    long ai = store->act_store(act);
    if (ai == cur_act) cur_high = i;
    else {
      inters.push_back({cur_low, cur_high, cur_act});
      cur_low = i;
      cur_high = i;
      cur_act = ai;
    }
  }
  inters.push_back({cur_low, cur_high, cur_act});
  return {inters, store};
}

IntCases sort_int_lambda_list(const IntCases& l) {
  return ocaml_list::stable_sort(
      [](const std::pair<long, lam>& a, const std::pair<long, lam>& b) {
        return a.first < b.first ? -1 : b.first < a.first ? 1 : 0;
      },
      l);
}

struct Interval {
  std::pair<long, long> edges;
  SwCases cases;
  std::shared_ptr<StoreExp> actions;
};
Interval as_interval(lam fail, long low, long high, const IntCases& l0) {
  IntCases l = sort_int_lambda_list(l0);
  // the pair right to left: the store first
  auto [cases, store] = fail ? as_interval_canfail(fail, low, high, l) : as_interval_nofail(l);
  return {get_edges(low, high, l), cases, store};
}

lam call_switcher(const L::ScopedLocation& loc, lam fail, lam arg, long low, long high, const IntCases& int_lambda_list) {
  Interval iv = as_interval(fail, low, high, int_lambda_list);
  return Switcher::zyva(loc, iv.edges, arg, iv.cases, *iv.actions);
}
lam call_switcher(const L::ScopedLocation& loc, lam fail, lam arg, const IntCases& l) {
  return call_switcher(loc, fail, arg, switch_::ocaml_min_int, switch_::ocaml_max_int, l);
}

Pat list_as_pat(const Pats& pats, std::size_t from = 0) {
  if (from >= pats.size()) fatal_error("Matching.list_as_pat");
  if (from + 1 == pats.size()) return pats[from];
  Pat pat = pats[from];
  return with_desc(pat, mkd(tt::Tpat_or{{PK::Tpat_or}, pat, list_as_pat(pats, from + 1), nullptr}));
}

// a constructor_description pattern_data: [cstr] with the pattern data of [data]
struct ConstrPat {
  const ConstructorDescription* cstr;
  Pat data;
};
Pats complete_pats_constrs(const std::vector<ConstrPat>& constrs) {
  if (constrs.empty()) fatal_error("Matching.complete_pats_constrs");
  const ConstrPat& constr = constrs[0];
  std::vector<const ConstructorDescription*> used;
  for (auto& c : constrs) used.push_back(c.cstr);
  Pats r;
  for (auto* cstr : parmatch::complete_constrs(constr.data->pat_env, constr.cstr, used)) {
    Head* h = head_of(constr.data, HK::Construct);
    h->cstr = cstr;
    r.push_back(to_omega_pattern(h));
  }
  return r;
}

std::pair<lam, Jumps> comp_final_exit(const DefaultEnv& def) {
  return {default_env::raise_final_exit(def), jumps::empty(Partial::Partial)};
}

std::optional<std::pair<lam, Jumps>> comp_exit(const Partiality& partial, const Context& ctx, const DefaultEnv& def) {
  if (auto p = default_env::pop(def)) {
    long i = p->first.first;
    return std::make_pair(lstaticraise(i), jumps::singleton(i, ctx));
  }
  // If we know that we are in Total match, we do not need to generate a
  // final exit in this case.
  if (partial.global == Partial::Total) return std::nullopt;
  return comp_final_exit(def);
}

// the trap handler to jump to in case of failure of elementary tests
std::pair<lam, Jumps> mk_failaction_neg(const ArgPartiality& arg_partial, const Context& ctx, const DefaultEnv& def) {
  if (arg_partial.p.current == Partial::Total) return {nullptr, jumps::empty(Partial::Total)};
  auto r = comp_exit(arg_partial.p, ctx, def);
  if (!r) return {nullptr, jumps::empty(Partial::Total)};
  return *r;
}

using CstrCases = std::vector<std::pair<const ConstructorDescription*, lam>>;

struct FailPos {
  lam fail;
  CstrCases fails;
  Jumps jumps;
};
FailPos mk_failaction_pos(const ArgPartiality& arg_partial, const std::vector<ConstrPat>& seen, const Context& ctx,
                          const DefaultEnv& defs0) {
  // The failure patterns are formed of the constructors not present in
  // [seen].
  Pats input_fail_pats = complete_pats_constrs(seen);
  if (static_cast<long>(input_fail_pats.size()) >= match_context_rows) {
    // Too many non-matched constructors -> reduced information.
    auto [fail, jumps] = mk_failaction_neg(arg_partial, ctx, defs0);
    return {fail, {}, jumps};
  }
  std::vector<std::pair<Pat, Context>> fail_pats_in_ctx;
  for (Pat pat : input_fail_pats) {
    Context pat_ctx = context::lub(pat, ctx);
    if (!context::is_empty(pat_ctx)) fail_pats_in_ctx.push_back({pat, pat_ctx});
  }
  auto mk_fails = [](const Pats& fail_pats, lam action) {
    CstrCases r;
    for (Pat pat : fail_pats) r.push_back({get_key_constr(pat), action});
    return r;
  };
  // We compare our failure patterns against our default environment; for
  // each failure pattern we compute a good exit.
  std::function<std::pair<CstrCases, Jumps>(const DefaultEnv&, const std::vector<std::pair<Pat, Context>>&)>
      fails_and_jumps = [&](const DefaultEnv& defs, const std::vector<std::pair<Pat, Context>>& fpic)
      -> std::pair<CstrCases, Jumps> {
    if (fpic.empty()) return {{}, jumps::empty(Partial::Total)};
    if (auto p = default_env::pop(defs)) {
      long idef = p->first.first;
      const Matrix& pss = p->first.second;
      const DefaultEnv& rem = p->second;
      Pats now;
      std::vector<std::pair<Pat, Context>> later;
      for (auto& fp : fpic) {
        if (context::matches(fp.second, pss)) now.push_back(fp.first);
        else later.push_back(fp);
      }
      if (now.empty()) return fails_and_jumps(rem, later);
      auto [fails, jmps] = fails_and_jumps(rem, later);
      CstrCases fails2 = mk_fails(now, lstaticraise(idef));
      fails2.insert(fails2.end(), fails.begin(), fails.end());
      Pat fail_pat = list_as_pat(now);
      Context fail_ctx = context::lub(fail_pat, ctx);
      return {fails2, jumps::add(idef, fail_ctx, jmps)};
    }
    if (arg_partial.p.global == Partial::Total)
      // all missing values are either ill-typed or handled by a matrix of
      // the default environment
      return {{}, jumps::empty(Partial::Total)};
    // in [Partial] mode, remaining failing patterns go to the final exit.
    Pats final_pats;
    for (auto& fp : fpic) final_pats.push_back(fp.first);
    return {mk_fails(final_pats, default_env::raise_final_exit(defs)), jumps::empty(Partial::Partial)};
  };
  auto [fails, jmps] = fails_and_jumps(defs0, fail_pats_in_ctx);
  return {nullptr, fails, jmps};
}

// the compiled division: (key * lambda) list, jumps, new discriminants
template <class Key>
struct CDiv {
  std::vector<std::pair<Key, lam>> cases;
  Jumps total;
  Pats pats;
};

std::pair<lam, Jumps> combine_constant(const L::ScopedLocation& loc, lam arg, const tt::Constant& cst,
                                       const ArgPartiality& partial, const Context& ctx, const DefaultEnv& def,
                                       const CDiv<tt::Constant>& c_div) {
  auto [fail, local_jumps] = mk_failaction_neg(partial, ctx, def);
  const ConstCases& const_lambda_list = c_div.cases;
  lam lambda1;
  switch (cst.kind) {
    case CK::Const_int: {
      IntCases l;
      for (auto& [c, act] : const_lambda_list) l.push_back({c.i, act});
      lambda1 = call_switcher(loc, fail, arg, l);
      break;
    }
    case CK::Const_char: {
      IntCases l;
      for (auto& [c, act] : const_lambda_list) l.push_back({c.i, act});
      lambda1 = call_switcher(loc, fail, arg, 0, 255, l);
      break;
    }
    case CK::Const_string: {
      // the clauses of stringswitch are sorted with duplicates removed
      ConstCases sorted = sort_lambda_list(const_lambda_list);
      std::vector<std::pair<std::string_view, lam>> sw;
      for (auto& [c, act] : sorted) sw.push_back({c.s, act});
      Shared3<std::string_view> s = share_actions_tree(sw, fail);
      std::vector<L::StringCase> cases;
      for (auto& [str, act] : s.sw) cases.push_back({str, act});
      lambda1 = s.hs(L::lstringswitch(arg, slice(cases), s.d, loc));
      break;
    }
    case CK::Const_float:
      lambda1 = make_test_sequence(loc, fail, pfloatcomp(L::FloatComparison::CFneq),
                                   pfloatcomp(L::FloatComparison::CFlt), arg, const_lambda_list);
      break;
    case CK::Const_int32:
      lambda1 = make_test_sequence(loc, fail, pbintcomp(BoxedInteger::Pint32, L::IntegerComparison::Cne),
                                   pbintcomp(BoxedInteger::Pint32, L::IntegerComparison::Clt), arg, const_lambda_list);
      break;
    case CK::Const_int64:
      lambda1 = make_test_sequence(loc, fail, pbintcomp(BoxedInteger::Pint64, L::IntegerComparison::Cne),
                                   pbintcomp(BoxedInteger::Pint64, L::IntegerComparison::Clt), arg, const_lambda_list);
      break;
    case CK::Const_nativeint:
      lambda1 = make_test_sequence(loc, fail, pbintcomp(BoxedInteger::Pnativeint, L::IntegerComparison::Cne),
                                   pbintcomp(BoxedInteger::Pnativeint, L::IntegerComparison::Clt), arg,
                                   const_lambda_list);
      break;
  }
  return {lambda1, jumps::union_(local_jumps, c_div.total)};
}

std::pair<IntCases, IntCases> split_cases(const std::vector<std::pair<ConstructorTag, lam>>& tag_lambda_list) {
  IntCases consts, nonconsts;
  for (auto& [tag, act] : tag_lambda_list) switch (tag.kind) {
      case ConstructorTag::Kind::Cstr_constant: consts.push_back({tag.n, act}); break;
      case ConstructorTag::Kind::Cstr_block: nonconsts.push_back({tag.n, act}); break;
      case ConstructorTag::Kind::Cstr_unboxed: nonconsts.push_back({0, act}); break;
      case ConstructorTag::Kind::Cstr_extension: fatal_error("Matching.split_cases");
    }
  return {sort_int_lambda_list(consts), sort_int_lambda_list(nonconsts)};
}

lam transl_match_on_option(lam arg, const L::ScopedLocation& loc, lam if_some, lam if_none) {
  // Keeping the Pisint test would make the bytecode slightly worse, but it
  // lets the native compiler generate better code -- see #10681.
  if (clflags::native_code) return L::lifthenelse(lprim(L::prim(PrimK::Pisint), {arg}, loc), if_none, if_some);
  return L::lifthenelse(arg, if_some, if_none);
}

std::pair<lam, Jumps> combine_extension_constructor(const L::ScopedLocation& loc, lam arg, env::t pat_env,
                                                    const ArgPartiality& partial, const Context& ctx,
                                                    const DefaultEnv& def,
                                                    const CDiv<const ConstructorDescription*>& c_div) {
  auto [fail, local_jumps] = mk_failaction_neg(partial, ctx, def);
  std::vector<std::pair<Path::t, lam>> consts, nonconsts;
  for (auto& [cstr, act] : c_div.cases) {
    if (cstr->cstr_tag.kind != ConstructorTag::Kind::Cstr_extension) fatal_error("Matching.split_extension_cases");
    (cstr->cstr_tag.ext_constant ? consts : nonconsts).push_back({cstr->cstr_tag.ext_path, act});
  }
  lam deflt;
  if (!fail) {
    if (!nonconsts.empty()) {
      deflt = nonconsts[0].second;
      nonconsts.erase(nonconsts.begin());
    } else if (!consts.empty()) {
      deflt = consts[0].second;
      consts.erase(consts.begin());
    } else
      fatal_error("Matching.combine_extension_constructor");
  } else
    deflt = fail;
  lam nonconst_lambda;
  if (nonconsts.empty()) nonconst_lambda = deflt;
  else {
    Ident::t tag = Ident::create_local("tag");
    lam tests = deflt;
    for (auto it = nonconsts.rbegin(); it != nonconsts.rend(); ++it) {
      lam ext = L::transl_extension_path(loc, pat_env, it->first);
      tests = L::lifthenelse(lprim(pintcomp(L::IntegerComparison::Ceq), {L::lvar(tag), ext}, loc), it->second, tests);
    }
    nonconst_lambda =
        L::llet(LetKind::Alias, L::ValueKind::gen(), tag,
                lprim(pfield(0, L::ImmediateOrPointer::Pointer, MutableFlag::Immutable), {arg}, loc), tests);
  }
  lam lambda1 = nonconst_lambda;
  for (auto it = consts.rbegin(); it != consts.rend(); ++it) {
    lam ext = L::transl_extension_path(loc, pat_env, it->first);
    lambda1 = L::lifthenelse(lprim(pintcomp(L::IntegerComparison::Ceq), {arg, ext}, loc), it->second, lambda1);
  }
  return {lambda1, jumps::union_(local_jumps, c_div.total)};
}

std::pair<lam, Jumps> combine_regular_constructor(const L::ScopedLocation& loc, lam arg,
                                                  const ConstructorDescription* cstr, const ArgPartiality& partial,
                                                  const Context& ctx, const DefaultEnv& def,
                                                  const CDiv<const ConstructorDescription*>& c_div) {
  // Regular concrete type
  long ncases = static_cast<long>(c_div.cases.size());
  long nconstrs = cstr->cstr_consts + cstr->cstr_nonconsts;
  bool sig_complete = ncases == nconstrs;
  lam fail_opt = nullptr;
  CstrCases fails;
  Jumps local_jumps = jumps::empty(Partial::Total);
  if (!sig_complete) {
    std::vector<ConstrPat> constrs;
    for (std::size_t k = 0; k < c_div.cases.size(); ++k) constrs.push_back({c_div.cases[k].first, c_div.pats[k]});
    FailPos fp = mk_failaction_pos(partial, constrs, ctx, def);
    fail_opt = fp.fail;
    fails = std::move(fp.fails);
    local_jumps = std::move(fp.jumps);
  }
  CstrCases descr_lambda_list = fails;
  descr_lambda_list.insert(descr_lambda_list.end(), c_div.cases.begin(), c_div.cases.end());
  std::vector<std::pair<ConstructorTag, lam>> tags;
  for (auto& [c, act] : descr_lambda_list) tags.push_back({c->cstr_tag, act});
  auto [consts, nonconsts] = split_cases(tags);
  lam lambda1;
  lam same = same_actions(descr_lambda_list);
  if (!fail_opt && same) {
    // Identical actions, no failure: 0 control-flow instructions.
    lambda1 = same;
  } else if (cstr->cstr_consts == 1 && cstr->cstr_nonconsts == 1 && consts.size() == 1 && consts[0].first == 0 &&
             nonconsts.size() == 1 && nonconsts[0].first == 0) {
    // This case is very frequent, it corresponds to options and lists.
    lambda1 = transl_match_on_option(arg, loc, nonconsts[0].second, consts[0].second);
  } else if (cstr->cstr_nonconsts == 0 && nonconsts.empty()) {
    // The matched type defines constant constructors only.
    lambda1 = call_switcher(loc, fail_opt, arg, 0, cstr->cstr_consts - 1, consts);
  } else {
    long n = cstr->cstr_consts;
    lam act0;  // = Some act when all non-const constructors match to act
    if (fail_opt && nonconsts.empty()) act0 = fail_opt;
    else if (fail_opt) act0 = static_cast<long>(nonconsts.size()) == cstr->cstr_nonconsts ? same_actions(nonconsts) : nullptr;
    else act0 = same_actions(nonconsts);
    if (act0) {
      lambda1 = L::lifthenelse(lprim(L::prim(PrimK::Pisint), {arg}, loc),
                               call_switcher(loc, fail_opt, arg, 0, n - 1, consts), act0);
    } else {
      // In the general case, emit a switch.
      std::vector<L::SwitchCase> c2, b2;
      for (auto& [k, a] : consts) c2.push_back({k, a});
      for (auto& [k, a] : nonconsts) b2.push_back({k, a});
      L::LambdaSwitch sw;
      sw.sw_numconsts = cstr->cstr_consts;
      sw.sw_consts = slice(c2);
      sw.sw_numblocks = cstr->cstr_nonconsts;
      sw.sw_blocks = slice(b2);
      sw.sw_failaction = fail_opt;
      auto [hs, sw2] = share_actions_sw(sw);
      L::LambdaSwitch sw3 = reintroduce_fail(sw2);
      lambda1 = hs(L::lswitch(arg, sw3, loc));
    }
  }
  return {lambda1, jumps::union_(local_jumps, c_div.total)};
}

std::pair<lam, Jumps> combine_constructor(const L::ScopedLocation& loc, lam arg, env::t pat_env,
                                          const ConstructorDescription* cstr, const ArgPartiality& partial,
                                          const Context& ctx, const DefaultEnv& def,
                                          const CDiv<const ConstructorDescription*>& actions) {
  if (cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension)
    return combine_extension_constructor(loc, arg, pat_env, partial, ctx, def, actions);
  return combine_regular_constructor(loc, arg, cstr, partial, ctx, def, actions);
}

lam make_test_sequence_variant_constant(lam fail, lam arg, const IntCases& int_lambda_list) {
  Interval iv = as_interval(fail, switch_::ocaml_min_int, switch_::ocaml_max_int, int_lambda_list);
  return Switcher::test_sequence(arg, iv.cases, *iv.actions);
}

lam call_switcher_variant_constant(const L::ScopedLocation& loc, lam fail, lam arg, const IntCases& l) {
  return call_switcher(loc, fail, arg, l);
}

lam call_switcher_variant_constr(const L::ScopedLocation& loc, lam fail, lam arg, const IntCases& l) {
  Ident::t v = Ident::create_local("variant");
  lam sw = call_switcher(loc, fail, L::lvar(v), l);
  return L::llet(LetKind::Alias, L::ValueKind::gen(), v,
                 lprim(pfield(0, L::ImmediateOrPointer::Pointer, MutableFlag::Immutable), {arg}, loc), sw);
}

std::pair<lam, Jumps> combine_variant(const L::ScopedLocation& loc, const RowDesc* row, lam arg,
                                      const ArgPartiality& partial, const Context& ctx, const DefaultEnv& def,
                                      const CDiv<VariantKey>& c_div) {
  long num_constr = 0;
  if (row_closed(row)) {
    for (auto& f : row_fields(row)) {
      RowFieldView v = row_field_repr(f.field);
      if (v.kind == RowFieldView::Kind::Rabsent ||
          (v.kind == RowFieldView::Kind::Reither && v.constant && !v.arg_types.empty()))
        continue;
      ++num_constr;
    }
  } else
    num_constr = switch_::ocaml_max_int;
  auto test_int_or_block = [&](lam a, lam if_int, lam if_block) {
    return L::lifthenelse(lprim(L::prim(PrimK::Pisint), {a}, loc), if_int, if_block);
  };
  std::vector<std::pair<ConstructorTag, lam>> tag_lambda_list;
  for (auto& [k, act] : c_div.cases) {
    ConstructorTag t{k.block ? ConstructorTag::Kind::Cstr_block : ConstructorTag::Kind::Cstr_constant, k.tag};
    tag_lambda_list.push_back({t, act});
  }
  bool sig_complete = static_cast<long>(tag_lambda_list.size()) == num_constr;
  lam one_action = same_actions(tag_lambda_list);
  lam fail = nullptr;
  Jumps local_jumps = jumps::empty(Partial::Total);
  if (!(sig_complete || partial.p.current == Partial::Total)) {
    auto r = mk_failaction_neg(partial, ctx, def);
    fail = r.first;
    local_jumps = std::move(r.second);
  }
  auto [consts, nonconsts] = split_cases(tag_lambda_list);
  lam lambda1;
  if (!fail && one_action) lambda1 = one_action;
  else if (consts.size() == 1 && nonconsts.size() == 1 && !fail)
    lambda1 = test_int_or_block(arg, consts[0].second, nonconsts[0].second);
  else if (nonconsts.empty()) {
    lam l = make_test_sequence_variant_constant(fail, arg, consts);
    // PR#11587: Switcher.test_sequence expects integer inputs, so if the
    // type allows pointers we must filter them away.
    lambda1 = fail ? test_int_or_block(arg, l, fail) : l;
  } else if (consts.empty()) {
    lam l = call_switcher_variant_constr(loc, fail, arg, nonconsts);
    // One must not dereference integers
    lambda1 = fail ? test_int_or_block(arg, fail, l) : l;
  } else {
    lam lam_const = call_switcher_variant_constant(loc, fail, arg, consts);
    lam lam_nonconst = call_switcher_variant_constr(loc, fail, arg, nonconsts);
    lambda1 = test_int_or_block(arg, lam_const, lam_nonconst);
  }
  return {lambda1, jumps::union_(local_jumps, c_div.total)};
}

std::pair<lam, Jumps> combine_array(const L::ScopedLocation& loc, lam arg, L::ArrayKind kind,
                                    const ArgPartiality& partial, const Context& ctx, const DefaultEnv& def,
                                    const CDiv<long>& c_div) {
  auto [fail, local_jumps] = mk_failaction_neg(partial, ctx, def);
  Ident::t newvar = Ident::create_local("len");
  lam sw = call_switcher(loc, fail, L::lvar(newvar), 0, switch_::ocaml_max_int, c_div.cases);
  lam lambda1 = L::bind(LetKind::Alias, newvar, lprim(parray(PrimK::Parraylength, kind), {arg}, loc), sw);
  return {lambda1, jumps::union_(local_jumps, c_div.total)};
}

// Insertion of debugging events
lam event_branch(L::IntRef* repr, lam l) {
  if (!repr) return l;
  if (auto* e = L::as<L::Levent>(l)) {
    repr->contents += 1;
    auto* ev = make<L::LambdaEvent>(*e->ev);
    ev->lev_repr = repr;
    return L::levent(e->l, ev);
  }
  if (auto* lt = L::as<L::Llet>(l)) return L::llet(lt->str, lt->k, lt->id, lt->arg, event_branch(repr, lt->body));
  if (L::as<L::Lstaticraise>(l)) return l;
  fatal_error("Matching.event_branch");
}

// raised when the compiler cannot produce code because control cannot reach
// the compiled clause
struct Unused {};

using CompileFun = std::function<std::pair<lam, Jumps>(const Context&, const InitialPM&)>;

template <class Key>
CDiv<Key> compile_list(const CompileFun& compile_fun, const std::vector<std::pair<Key, CellP>>& division) {
  CDiv<Key> r;
  std::vector<Jumps> totals;  // head first
  for (auto& [key, cell] : division) {
    if (context::is_empty(cell->ctx)) continue;
    std::pair<lam, Jumps> res;
    try {
      res = compile_fun(cell->ctx, cell->pm);
    } catch (const Unused&) {
      continue;
    }
    totals.insert(totals.begin(), jumps::map(context::combine, res.second));
    r.cases.push_back({key, res.first});
    r.pats.push_back(to_omega_pattern(cell->discr));
  }
  r.total = jumps::unions(totals);
  return r;
}

std::pair<lam, Jumps> compile_orhandlers(const CompileFun& compile_fun, lam lambda1, const Jumps& total1,
                                         const Context& ctx, const std::vector<Handler>& to_catch) {
  lam r = lambda1;
  Jumps total_r = total1;
  for (auto& h : to_catch) {
    const Matrix& mat = h.provenance;
    long i = h.exit;
    Context hctx = context::select_columns(mat, ctx);
    std::pair<lam, Jumps> res;
    try {
      res = compile_fun(hctx, h.pm);
    } catch (const Unused&) {
      r = L::lstaticcatch(r, i, slice(h.vars), L::lambda_unit());
      continue;
    }
    auto& [handler_i, total_i] = res;
    lam raw = raw_action(r);
    if (auto* sr = L::as<L::Lstaticraise>(raw)) {
      if (i == sr->i) {
        // List.fold_right2 (bind_with_value_kind Alias) vars args handler_i
        if (h.vars.size() != sr->args.size()) fatal_error("Matching.compile_orhandlers");
        lam b = handler_i;
        for (std::size_t k = h.vars.size(); k-- > 0;)
          b = L::bind_with_value_kind(LetKind::Alias, h.vars[k].id, h.vars[k].kind, sr->args[k], b);
        long n = ncols(mat);
        return {b, jumps::map([n](const Context& c) { return context::rshift_num(n, c); }, total_i)};
      }
      continue;
    }
    long n = ncols(mat);
    r = L::lstaticcatch(r, i, slice(h.vars), handler_i);
    total_r = jumps::union_(jumps::remove(i, total_r),
                            jumps::map([n](const Context& c) { return context::rshift_num(n, c); }, total_i));
  }
  return {r, total_r};
}

template <class Key, class DivideF, class CombineF>
std::pair<lam, Jumps> compile_test(const CompileFun& compile_fun, const ArgPartiality& arg_partial, DivideF divide_f,
                                   CombineF combine, const Context& ctx, const SimplePM& to_match) {
  Division<Key> division = divide_f(ctx, to_match);
  CDiv<Key> c_div = compile_list(compile_fun, division.cells);
  if (c_div.cases.empty()) {
    auto [l, total] = mk_failaction_neg(arg_partial, ctx, to_match.def);
    if (!l) throw Unused{};
    return {l, total};
  }
  return combine(ctx, to_match.def, c_div);
}

// Attempt to avoid some useless bindings by lowering them
// Approximation of v present in lam
bool approx_present(Ident::t v, lam l) {
  switch (l->kind) {
    case LK::Lconst: return false;
    case LK::Lstaticraise:
      for (lam a : L::as<L::Lstaticraise>(l)->args)
        if (approx_present(v, a)) return true;
      return false;
    case LK::Lprim:
      for (lam a : L::as<L::Lprim>(l)->args)
        if (approx_present(v, a)) return true;
      return false;
    case LK::Llet: {
      auto* lt = L::as<L::Llet>(l);
      if (lt->str != LetKind::Alias) return true;
      return approx_present(v, lt->arg) || approx_present(v, lt->body);
    }
    case LK::Lvar: return ident::same(v, L::as<L::Lvar>(l)->id);
    default: return true;
  }
}

lam lower_bind(Ident::t v, lam arg, lam l) {
  if (auto* ite = L::as<L::Lifthenelse>(l)) {
    bool pcond = approx_present(v, ite->cond);
    bool pso = approx_present(v, ite->ifso);
    bool pnot = approx_present(v, ite->ifnot);
    if (!pcond && !pso && !pnot) return l;
    if (!pcond && pso && !pnot) return L::lifthenelse(ite->cond, lower_bind(v, arg, ite->ifso), ite->ifnot);
    if (!pcond && !pso && pnot) return L::lifthenelse(ite->cond, ite->ifso, lower_bind(v, arg, ite->ifnot));
    return L::bind(LetKind::Alias, v, arg, l);
  }
  if (auto* s = L::as<L::Lswitch>(l)) {
    if (s->sw.sw_consts.size() == 1 && s->sw.sw_blocks.empty() && !approx_present(v, s->arg)) {
      L::LambdaSwitch sw = s->sw;
      sw.sw_consts = slice(std::vector<L::SwitchCase>{{s->sw.sw_consts[0].key, lower_bind(v, arg, s->sw.sw_consts[0].action)}});
      return L::lswitch(s->arg, sw, s->loc);
    }
    if (s->sw.sw_consts.empty() && s->sw.sw_blocks.size() == 1 && !approx_present(v, s->arg)) {
      L::LambdaSwitch sw = s->sw;
      sw.sw_blocks = slice(std::vector<L::SwitchCase>{{s->sw.sw_blocks[0].key, lower_bind(v, arg, s->sw.sw_blocks[0].action)}});
      return L::lswitch(s->arg, sw, s->loc);
    }
    return L::bind(LetKind::Alias, v, arg, l);
  }
  if (auto* lt = L::as<L::Llet>(l); lt && lt->str == LetKind::Alias) {
    if (approx_present(v, lt->arg)) return L::bind(LetKind::Alias, v, arg, l);
    return L::llet(LetKind::Alias, lt->k, lt->id, lt->arg, lower_bind(v, arg, lt->body));
  }
  return L::bind(LetKind::Alias, v, arg, l);
}

lam bind_check(LetKind kind, Ident::t v, lam arg, lam l) {
  if (L::as<L::Lvar>(arg)) return L::bind(kind, v, arg, l);
  if (kind == LetKind::Alias) return lower_bind(v, arg, l);
  return L::bind(kind, v, arg, l);
}

template <class P>
using CompFun = std::function<std::pair<lam, Jumps>(const Partiality&, const Context&, const P&)>;

template <class P>
std::pair<lam, Jumps> comp_match_handlers(const CompFun<P>& comp_fun, const Partiality& partial, const Context& ctx,
                                          P first_match, std::vector<std::pair<long, P>> next_matches) {
  for (;;) {
    if (next_matches.empty()) return comp_fun(partial, ctx, first_match);
    Partiality p0 = partial;
    p0.current = Partial::Partial;
    std::pair<lam, Jumps> first;
    try {
      first = comp_fun(p0, ctx, first_match);
    } catch (const Unused&) {
      first_match = next_matches[0].second;
      next_matches.erase(next_matches.begin());
      continue;
    }
    lam body = first.first;
    Jumps jumps_body = first.second;
    for (std::size_t k = 0; k < next_matches.size(); ++k) {
      long i = next_matches[k].first;
      const P& pm_i = next_matches[k].second;
      // [c_rec] is only called on [Following] sub-matrices
      Partiality p1 = partial;
      p1.tempo = Temporality::Following;
      auto [ctx_i, jumps_rem] = jumps::extract(i, jumps_body);
      if (context::is_empty(ctx_i)) continue;
      // All those submatrices are [Partial], except possibly for the last
      // one.
      if (k + 1 < next_matches.size()) p1.current = Partial::Partial;
      std::pair<lam, Jumps> r;
      try {
        r = comp_fun(p1, ctx_i, pm_i);
      } catch (const Unused&) {
        body = L::lstaticcatch(body, i, {}, L::lambda_unit());
        jumps_body = jumps_rem;
        continue;
      }
      body = L::lstaticcatch(body, i, {}, r.first);
      jumps_body = jumps::union_(r.second, jumps_rem);
    }
    return {body, jumps_body};
  }
}

// To find reasonable names for variables
Ident::t name_pattern(const char* deflt, const std::vector<Clause>& cls) {
  for (auto& c : cls) {
    if (auto* v = as<tt::Tpat_var>(c.p->pat_desc)) return v->id;
    if (auto* a = as<tt::Tpat_alias>(c.p->pat_desc)) return a->id;
  }
  return Ident::create_local(deflt);
}
Ident::t arg_to_var(lam arg, const std::vector<Clause>& cls) {
  if (auto* v = L::as<L::Lvar>(arg)) return v->id;
  return name_pattern("*match*", cls);
}

ArgPartiality compute_arg_partial(const Partiality& partial, MutableFlag mut) {
  if (partial.tempo == Temporality::Following && mut == MutableFlag::Mutable) {
    Partiality p = partial;
    p.global = Partial::Partial;
    return {p};
  }
  return {partial};
}

MutableFlag mut_of_binding_kind(LetKind k) {
  return k == LetKind::StrictOpt ? MutableFlag::Mutable : MutableFlag::Immutable;
}

std::pair<lam, Jumps> bind_match_arg(LetKind kind, Ident::t v, lam arg, std::pair<lam, Jumps> lj) {
  Jumps jmps = mut_of_binding_kind(kind) == MutableFlag::Immutable ? std::move(lj.second)
                                                                  : jumps::map(context::erase_first_col, lj.second);
  return {bind_check(kind, v, arg, lj.first), std::move(jmps)};
}

using GeneralPM = PM<Args, Clause>;

std::pair<lam, Jumps> compile_match(scopes sc, L::IntRef* repr, const Partiality& partial, const Context& ctx,
                                    const InitialPM& m);
std::pair<lam, Jumps> compile_match_nonempty(scopes sc, L::IntRef* repr, const Partiality& partial,
                                             const Context& ctx, const GeneralPM& m);
std::pair<lam, Jumps> do_compile_matching(scopes sc, L::IntRef* repr, const Partiality& partial, const Context& ctx,
                                          const PmHalfP& pmh);

std::pair<lam, Jumps> combine_handlers(scopes sc, L::IntRef* repr, const Partiality& partial, const Context& ctx,
                                       PmHalfP first_match, const Nexts& rem) {
  CompFun<PmHalfP> f = [sc, repr](const Partiality& p, const Context& c, const PmHalfP& pmh) {
    return do_compile_matching(sc, repr, p, c, pmh);
  };
  return comp_match_handlers(f, partial, ctx, std::move(first_match), rem);
}

// The main compilation function.
std::pair<lam, Jumps> compile_match(scopes sc, L::IntRef* repr, const Partiality& partial, const Context& ctx,
                                    const InitialPM& m) {
  if (!m.cases.empty() && m.cases[0].ps.empty()) {
    lam action = m.cases[0].act;
    if (L::is_guarded(action)) {
      InitialPM rest{std::vector<InitialClause>(m.cases.begin() + 1, m.cases.end()), m.args, m.def};
      auto [l, total] = compile_match(sc, nullptr, partial, ctx, rest);
      return {event_branch(repr, L::patch_guarded(l, action)), total};
    }
    return {event_branch(repr, action), jumps::empty(Partial::Total)};
  }
  GeneralPM g{{}, m.args, m.def};
  for (auto& c : m.cases) g.cases.push_back(of_initial(c));
  return compile_match_nonempty(sc, repr, partial, ctx, g);
}

std::pair<lam, Jumps> compile_match_nonempty(scopes sc, L::IntRef* repr, const Partiality& partial,
                                             const Context& ctx, const GeneralPM& m) {
  if (m.cases.empty() && m.args.empty()) {
    auto exit = comp_exit(partial, ctx, m.def);
    if (!exit) fatal_error("Matching: impossible empty matrix in a Total match");
    return *exit;
  }
  if (m.args.empty()) fatal_error("Matching.compile_match_nonempty");
  const ArgT& first = m.args[0];
  lam arg = first.arg;
  Ident::t v = arg_to_var(arg, m.cases);
  SplitArgs args{FirstArg{PureArg{v, nullptr}, first.binding_kind, first.mut}, Args(m.args.begin() + 1, m.args.end())};
  lam lv = L::lvar(v);
  std::vector<Clause> cases;
  for (auto& c : m.cases) cases.push_back(half_simplify_nonempty(lv, c));
  SimplePM m2{std::move(cases), std::move(args), m.def};
  auto [first_match, rem] = split_and_precompile_half_simplified(m2);
  return bind_match_arg(first.binding_kind, v, arg, combine_handlers(sc, repr, partial, ctx, first_match, rem));
}

std::pair<lam, Jumps> compile_match_simplified(scopes sc, L::IntRef* repr, const Partiality& partial,
                                               const Context& ctx, const SimplePM& m) {
  auto [first_match, rem] = split_and_precompile_simplified(m);
  return combine_handlers(sc, repr, partial, ctx, first_match, rem);
}

std::pair<lam, Jumps> compile_no_test(const Cell& division, L::IntRef* repr, scopes sc, const Partiality& partial,
                                      const std::function<Context(const Context&)>& up_ctx) {
  auto [l, total] = compile_match(sc, repr, partial, division.ctx, division.pm);
  return {l, jumps::map(up_ctx, total)};
}

std::pair<lam, Jumps> do_compile_matching(scopes sc, L::IntRef* repr, const Partiality& partial, const Context& ctx,
                                          const PmHalfP& pmh) {
  switch (pmh->kind) {
    case PmHalf::K::Pm: {
      const SimplePM& pm = pmh->pm;
      const FirstArg& first = pm.args.first;
      lam arg = arg_of_pure(first.arg);
      // [arg_partial]: the partiality information used to compile the
      // upcoming switch on the first argument
      ArgPartiality arg_partial = compute_arg_partial(partial, first.mut);
      HeadP ph = what_is_cases(pm.cases);
      Pat pomega = to_omega_pattern(ph);
      L::ScopedLocation ploc = head_loc(sc, ph);
      CompileFun cm = [sc, repr, partial](const Context& c, const InitialPM& p) {
        return compile_match(sc, repr, partial, c, p);
      };
      switch (ph->kind) {
        case HK::Any: return compile_no_test(divide_var(ctx, pm), repr, sc, partial, context::rshift);
        case HK::Tuple: return compile_no_test(divide_tuple(sc, ph, ctx, pm), repr, sc, partial, context::combine);
        case HK::Record: {
          if (ph->lbls.empty()) fatal_error("Matching.do_compile_matching");
          return compile_no_test(divide_record(ph->lbls[0]->lbl_all, sc, ph, ctx, pm), repr, sc, partial,
                                 context::combine);
        }
        case HK::Constant: {
          tt::Constant cst = ph->c;
          return compile_test<tt::Constant>(
              cm, arg_partial, divide_constant,
              [&](const Context& c, const DefaultEnv& d, const CDiv<tt::Constant>& cd) {
                return combine_constant(ploc, arg, cst, arg_partial, c, d, cd);
              },
              ctx, pm);
        }
        case HK::Construct: {
          const ConstructorDescription* cstr = ph->cstr;
          return compile_test<const ConstructorDescription*>(
              cm, arg_partial, [sc](const Context& c, const SimplePM& p) { return divide_constructor(sc, c, p); },
              [&](const Context& c, const DefaultEnv& d, const CDiv<const ConstructorDescription*>& cd) {
                return combine_constructor(ploc, arg, ph->pat_env, cstr, arg_partial, c, d, cd);
              },
              ctx, pm);
        }
        case HK::Array: {
          L::ArrayKind kind = typeopt::array_pattern_kind(pomega);
          return compile_test<long>(
              cm, arg_partial,
              [sc, kind](const Context& c, const SimplePM& p) { return divide_array(sc, kind, c, p); },
              [&](const Context& c, const DefaultEnv& d, const CDiv<long>& cd) {
                return combine_array(ploc, arg, kind, arg_partial, c, d, cd);
              },
              ctx, pm);
        }
        case HK::Lazy: return compile_no_test(divide_lazy(sc, ph, ctx, pm), repr, sc, partial, context::combine);
        case HK::Variant: {
          const RowDesc* row = ph->cstr_row->contents;
          return compile_test<VariantKey>(
              cm, arg_partial,
              [sc, row](const Context& c, const SimplePM& p) { return divide_variant(sc, row, c, p); },
              [&](const Context& c, const DefaultEnv& d, const CDiv<VariantKey>& cd) {
                return combine_variant(ploc, row, arg, arg_partial, c, d, cd);
              },
              ctx, pm);
        }
      }
      fatal_error("Matching.do_compile_matching");
    }
    case PmHalf::K::PmVar: {
      auto [l, total] = do_compile_matching(sc, repr, partial, context::lshift(ctx), pmh->inside);
      return {l, jumps::map(context::rshift, total)};
    }
    case PmHalf::K::PmOr: {
      auto [l, total] = compile_match_simplified(sc, repr, partial, ctx, pmh->pm);
      CompileFun cm = [sc, repr, partial](const Context& c, const InitialPM& p) {
        return compile_match(sc, repr, partial, c, p);
      };
      return compile_orhandlers(cm, l, total, ctx, pmh->handlers);
    }
  }
  fatal_error("Matching.do_compile_matching");
}

// ---- the entry points ----
struct Failer {
  enum class K { Raise_match_failure, Reraise_noloc, Reperform_noloc } kind;
  std::vector<lam> lams;  // Reraise_noloc: [exn]; Reperform_noloc: the list
};

lam failure_handler(scopes sc, const Location& loc, const Failer& failer) {
  switch (failer.kind) {
    case Failer::K::Reperform_noloc: return lprim(L::prim(PrimK::Preperform), failer.lams, loc_unknown());
    case Failer::K::Reraise_noloc: return lprim(praise(L::RaiseKind::Raise_reraise), {failer.lams[0]}, loc_unknown());
    case Failer::K::Raise_match_failure: {
      L::ScopedLocation sloc = debuginfo::of_location(sc, loc);
      lam slot = L::transl_extension_path(sloc, env::initial(), predef::paths().match_failure);
      const Position& pos = loc.loc_start;
      auto* blk = make<L::StructuredConstant>();
      blk->kind = L::StructuredConstant::Kind::Const_block;
      blk->i = 0;
      auto* fname = make<L::StructuredConstant>();
      fname->kind = L::StructuredConstant::Kind::Const_immstring;
      fname->s = pos.pos_fname;
      blk->fields = slice(std::vector<const L::StructuredConstant*>{fname, L::const_int(pos.pos_lnum),
                                                                     L::const_int(pos.pos_cnum - pos.pos_bol)});
      return lprim(praise(L::RaiseKind::Raise_regular),
                   {lprim(pmakeblock_immutable(0), {slot, L::lconst(blk)}, sloc)}, sloc);
    }
  }
  fatal_error("Matching.failure_handler");
}

template <class A, class C>
lam toplevel_handler(scopes sc, const Location& loc, const Failer& failer, Partial partial0, const A& args,
                     const std::vector<C>& cases,
                     const std::function<std::pair<lam, Jumps>(const Partiality&, const PM<A, C>&)>& compile_fun) {
  long final_exit = L::next_raise_count();
  DefaultEnv def = default_env::empty(final_exit);
  PM<A, C> pm{cases, args, def};
  // Example: [function _ -> .]
  bool only_refutations = cases.empty();
  Partial p = only_refutations || safer_matching ? Partial::Partial : partial0;
  Partiality partial{p, p, Temporality::First};
  std::pair<lam, Jumps> r;
  try {
    r = compile_fun(partial, pm);
  } catch (const Unused&) {
    fatal_error("Matching.toplevel_handler");
  }
  if (jumps::partial(r.second) == Partial::Total) return r.first;
  if (partial.global == Partial::Total) {
    // The type-checker believed the pattern-matching to be Total, but the
    // compiler found it to be Partial.  (Warnings.Degraded_to_partial_match
    // is not emitted yet: warning messages are stage 9.)
  }
  return L::lstaticcatch(r.first, final_exit, {}, failure_handler(sc, loc, failer));
}

// The mutability information denotes the mutability of a *position*; at the
// root we are immutable.
ArgT root_arg(lam arg, LetKind binding_kind) { return ArgT{arg, binding_kind, MutableFlag::Immutable}; }

lam compile_matching(scopes sc, const Location& loc, const Failer& failer, L::IntRef* repr, lam arg,
                     Slice<PatAction> pat_act_list, Partial partial) {
  Args args{root_arg(arg, LetKind::Strict)};
  std::vector<Clause> rows;
  for (auto& pa : pat_act_list) rows.push_back({pa.pat, {}, pa.action});
  return toplevel_handler<Args, Clause>(sc, loc, failer, partial, args, rows,
                                        [sc, repr](const Partiality& p, const GeneralPM& pm) {
                                          return compile_match_nonempty(sc, repr, p, context::start(1), pm);
                                        });
}

lam simple_for_let(scopes sc, const Location& loc, lam param, Pat pat, lam body) {
  std::vector<PatAction> l{{pat, body}};
  return compile_matching(sc, loc, Failer{Failer::K::Raise_match_failure, {}}, nullptr, param, slice(l),
                          Partial::Partial);
}

// Optimize binding of immediate tuples (see matching.ml's for_let)
lam map_return(const std::function<lam(lam)>& f, lam l) {
  switch (l->kind) {
    case LK::Llet: {
      auto* x = L::as<L::Llet>(l);
      return L::llet(x->str, x->k, x->id, x->arg, map_return(f, x->body));
    }
    case LK::Lmutlet: {
      auto* x = L::as<L::Lmutlet>(l);
      return L::lmutlet(x->k, x->id, x->arg, map_return(f, x->body));
    }
    case LK::Lletrec: {
      auto* x = L::as<L::Lletrec>(l);
      return L::lletrec(x->decl, map_return(f, x->body));
    }
    case LK::Lifthenelse: {
      auto* x = L::as<L::Lifthenelse>(l);
      lam e = map_return(f, x->ifnot);
      lam t = map_return(f, x->ifso);
      return L::lifthenelse(x->cond, t, e);
    }
    case LK::Lsequence: {
      auto* x = L::as<L::Lsequence>(l);
      return L::lsequence(x->l1, map_return(f, x->l2));
    }
    case LK::Levent: {
      auto* x = L::as<L::Levent>(l);
      return L::levent(map_return(f, x->l), x->ev);
    }
    case LK::Ltrywith: {
      auto* x = L::as<L::Ltrywith>(l);
      lam h = map_return(f, x->handler);
      lam b = map_return(f, x->body);
      return L::ltrywith(b, x->exn, h);
    }
    case LK::Lstaticcatch: {
      auto* x = L::as<L::Lstaticcatch>(l);
      lam h = map_return(f, x->handler);
      lam b = map_return(f, x->body);
      return L::lstaticcatch(b, x->i, x->params, h);
    }
    case LK::Lswitch: {
      auto* x = L::as<L::Lswitch>(l);
      auto map_cases = [&](Slice<L::SwitchCase> cases) {
        std::vector<L::SwitchCase> r;
        for (auto& c : cases) r.push_back({c.key, map_return(f, c.action)});
        return slice(r);
      };
      // the record fields right to left
      L::LambdaSwitch sw = x->sw;
      sw.sw_failaction = x->sw.sw_failaction ? map_return(f, x->sw.sw_failaction) : nullptr;
      sw.sw_blocks = map_cases(x->sw.sw_blocks);
      sw.sw_consts = map_cases(x->sw.sw_consts);
      return L::lswitch(x->arg, sw, x->loc);
    }
    case LK::Lstringswitch: {
      auto* x = L::as<L::Lstringswitch>(l);
      lam d = x->def ? map_return(f, x->def) : nullptr;
      std::vector<L::StringCase> cases;
      for (auto& c : x->cases) cases.push_back({c.s, map_return(f, c.action)});
      return L::lstringswitch(x->arg, slice(cases), d, x->loc);
    }
    case LK::Lstaticraise: return l;
    case LK::Lprim:
      if (L::as<L::Lprim>(l)->p.kind == PrimK::Praise) return l;
      return f(l);
    default: return f(l);
  }
}

// The 'opt' reference indicates if the optimization is worthy.
lam assign_pat(scopes sc, bool& opt, long nraise, const std::vector<Ident::t>& catch_ids, const Location& loc,
               Pat pat0, lam l0) {
  struct Sublet {
    std::vector<std::pair<Ident::t, Ident::t>> ids;
    Pat pat;
    lam l;
  };
  std::vector<Sublet> acc;  // in collect order: the leftmost first
  std::function<void(Pat, lam)> collect = [&](Pat pat, lam l) {
    if (auto* t = as<tt::Tpat_tuple>(pat->pat_desc)) {
      if (auto* p = L::as<L::Lprim>(l); p && p->p.kind == PrimK::Pmakeblock) {
        opt = true;
        if (t->pats.size() != p->args.size()) throw std::invalid_argument("List.fold_left2");
        for (std::size_t k = 0; k < t->pats.size(); ++k) collect(t->pats[k].pat, p->args[k]);
        return;
      }
      if (auto* c = L::as<L::Lconst>(l); c && c->c->kind == L::StructuredConstant::Kind::Const_block) {
        opt = true;
        if (t->pats.size() != c->c->fields.size()) throw std::invalid_argument("List.fold_left2");
        for (std::size_t k = 0; k < t->pats.size(); ++k) collect(t->pats[k].pat, L::lconst(c->c->fields[k]));
        return;
      }
    }
    // pattern idents will be bound in staticcatch (let body), so we refresh
    // them here to guarantee binders uniqueness
    std::vector<Ident::t> pat_ids = tt::pat_bound_idents(pat);
    std::vector<std::pair<Ident::t, Ident::t>> fresh_ids;
    for (Ident::t id : pat_ids) fresh_ids.push_back({id, ident::rename(id)});
    acc.push_back({fresh_ids, tt::alpha_pat(fresh_ids, pat), l});
  };
  collect(pat0, l0);
  // rev_sublets: the leftmost tuple pattern first (acc is in that order)
  const std::vector<Sublet>& rev_sublets = acc;
  L::IdentMap<Ident::t> tbl;
  for (auto& s : rev_sublets)
    for (auto& [id, fresh] : s.ids) tbl[id] = fresh;
  std::vector<lam> vars;
  for (Ident::t id : catch_ids) {
    auto it = tbl.find(id);
    if (it == tbl.end()) throw std::out_of_range("Ident.find_same");
    vars.push_back(L::lvar(it->second));
  }
  lam code = lstaticraise(nraise, vars);
  for (auto& s : rev_sublets) code = simple_for_let(sc, loc, s.l, s.pat, code);
  return code;
}

lam for_let_(scopes sc, const Location& loc, lam param, Pat pat, lam body) {
  if (pat->pat_desc->kind == PK::Tpat_any)
    // This eliminates a useless variable (and stack slot in bytecode) for
    // "let _ = ...". See #6865.
    return L::lsequence(param, body);
  if (auto* v = as<tt::Tpat_var>(pat->pat_desc)) {
    // Fast path, and keep track of simple bindings to unboxable numbers.
    L::ValueKind k = typeopt::value_kind(pat->pat_env, pat->pat_type);
    return L::llet(LetKind::Strict, k, v->id, param, body);
  }
  bool opt = false;
  long nraise = L::next_raise_count();
  std::vector<tt::BoundIdent> catch_ids = tt::pat_bound_idents_full(pat);
  std::vector<L::Param> ids_with_kinds;
  for (auto& b : catch_ids) ids_with_kinds.push_back({b.id, typeopt::value_kind(pat->pat_env, b.ty)});
  std::vector<Ident::t> ids;
  for (auto& b : catch_ids) ids.push_back(b.id);
  lam bind = map_return([&](lam l) { return assign_pat(sc, opt, nraise, ids, loc, pat, l); }, param);
  if (opt) return L::lstaticcatch(bind, nraise, slice(ids_with_kinds), body);
  return simple_for_let(sc, loc, param, pat, body);
}

Pats flatten_simple_pattern(long size, Pat p) {
  if (auto* t = as<tt::Tpat_tuple>(p->pat_desc)) {
    Pats ps;
    for (auto& x : t->pats) ps.push_back(x.pat);
    return ps;
  }
  if (p->pat_desc->kind == PK::Tpat_any) return omegas(size);
  // All calls to this function originate from [do_for_multiple_match],
  // where we know that the scrutinee is a tuple literal.
  fatal_error("Matching.flatten_pattern");
}

std::vector<Clause> flatten_cases(long size, const std::vector<Clause>& cases) {
  std::vector<Clause> r;
  for (auto& c : cases) {
    if (!c.ps.empty()) fatal_error("Matching.flatten_hc_cases");
    Pats ps = flatten_simple_pattern(size, c.p);
    if (ps.empty()) fatal_error("Matching.flatten_cases");
    r.push_back({ps[0], Pats(ps.begin() + 1, ps.end()), c.act});
  }
  return r;
}

GeneralPM flatten_pm(long size, const Args& args, const SimplePM& pm) {
  return GeneralPM{flatten_cases(size, pm.cases), args, default_env::flatten(size, pm.def)};
}

Handler flatten_handler(long size, const Handler& h) {
  Handler r = h;
  r.provenance = flatten_matrix(size, h.provenance);
  return r;
}

// pm_flattened = FPmOr of (args, pattern, unit) pm_or_compiled | FPm of ..
struct FPm {
  bool is_or;
  GeneralPM body;
  std::vector<Handler> handlers;
};
using FPmP = std::shared_ptr<const FPm>;

FPmP flatten_precompiled(long size, const Args& args, const PmHalf& pmh) {
  auto r = std::make_shared<FPm>();
  switch (pmh.kind) {
    case PmHalf::K::Pm:
      r->is_or = false;
      r->body = flatten_pm(size, args, pmh.pm);
      return r;
    case PmHalf::K::PmOr:
      r->is_or = true;
      r->body = flatten_pm(size, args, pmh.pm);
      for (auto& h : pmh.handlers) r->handlers.push_back(flatten_handler(size, h));
      return r;
    case PmHalf::K::PmVar: break;
  }
  fatal_error("Matching.flatten_precompiled");
}

std::pair<lam, Jumps> compile_flattened(scopes sc, L::IntRef* repr, const Partiality& partial, const Context& ctx,
                                        const FPmP& pmh) {
  if (!pmh->is_or) return compile_match_nonempty(sc, repr, partial, ctx, pmh->body);
  auto [l, total] = compile_match_nonempty(sc, repr, partial, ctx, pmh->body);
  CompileFun cm = [sc, repr, partial](const Context& c, const InitialPM& p) {
    return compile_match(sc, repr, partial, c, p);
  };
  return compile_orhandlers(cm, l, total, ctx, pmh->handlers);
}

lam do_for_multiple_match(scopes sc, const Location& loc, const std::vector<Ident::t>& idl,
                          Slice<PatAction> pat_act_list, Partial partial) {
  L::IntRef* repr = nullptr;
  L::ScopedLocation sloc = debuginfo::of_location(sc, loc);
  std::vector<lam> largs;
  for (Ident::t id : idl) largs.push_back(L::lvar(id));
  lam arg = lprim(pmakeblock_immutable(0), largs, sloc);
  SplitArgs input_args{FirstArg{PureArg{nullptr, arg}, LetKind::Strict, MutableFlag::Immutable}, {}};
  std::vector<Clause> rows;
  for (auto& pa : pat_act_list) rows.push_back({pa.pat, {}, pa.action});
  using SPM = PM<SplitArgs, Clause>;
  return toplevel_handler<SplitArgs, Clause>(
      sc, loc, Failer{Failer::K::Raise_match_failure, {}}, partial, input_args, rows,
      [&](const Partiality& p, const SPM& pm1) {
        SimplePM pm1_half{{}, pm1.args, pm1.def};
        for (auto& c : pm1.cases) pm1_half.cases.push_back(half_simplify_nonempty(arg, c));
        auto [next, nexts] = split_and_precompile_half_simplified(pm1_half);
        long size = static_cast<long>(idl.size());
        Args args;
        for (Ident::t id : idl) args.push_back(root_arg(L::lvar(id), LetKind::Alias));
        FPmP flat_next = flatten_precompiled(size, args, *next);
        std::vector<std::pair<long, FPmP>> flat_nexts;
        for (auto& [e, pm] : nexts) flat_nexts.push_back({e, flatten_precompiled(size, args, *pm)});
        CompFun<FPmP> f = [sc, repr](const Partiality& pp, const Context& c, const FPmP& x) {
          return compile_flattened(sc, repr, pp, c, x);
        };
        return comp_match_handlers(f, p, context::start(size), flat_next, flat_nexts);
      });
}

}  // namespace

// ---- exported ----------------------------------------------------------------------------

lam for_function(scopes sc, const Location& loc, L::IntRef* repr, lam param, Slice<PatAction> pat_act_list,
                 typedtree::Partial partial) {
  return compile_matching(sc, loc, Failer{Failer::K::Raise_match_failure, {}}, repr, param, pat_act_list, partial);
}

// In the following two cases, exhaustiveness info is not available!
lam for_trywith(scopes sc, const Location& loc, lam param, Slice<PatAction> pat_act_list) {
  // the failure action reraises without location information
  return compile_matching(sc, loc, Failer{Failer::K::Reraise_noloc, {param}}, nullptr, param, pat_act_list,
                          Partial::Partial);
}

lam for_handler(scopes sc, const Location& loc, lam param, lam cont, Slice<PatAction> pat_act_list) {
  return compile_matching(sc, loc, Failer{Failer::K::Reperform_noloc, {param, cont}}, nullptr, param, pat_act_list,
                          Partial::Partial);
}

lam for_let(scopes sc, const Location& loc, lam param, const typedtree::Pattern* pat, lam body) {
  return for_let_(sc, loc, param, pat, body);
}

// Easy case since variables are available
lam for_tupled_function(scopes sc, const Location& loc, Slice<Ident::t> paraml, Slice<PatsAction> pats_act_list,
                        typedtree::Partial partial) {
  Args args;
  for (Ident::t id : paraml) args.push_back(root_arg(L::lvar(id), LetKind::Strict));
  std::vector<InitialClause> rows;
  for (auto& pa : pats_act_list) rows.push_back({Pats(pa.pats.begin(), pa.pats.end()), pa.action});
  long n = static_cast<long>(paraml.size());
  return toplevel_handler<Args, InitialClause>(sc, loc, Failer{Failer::K::Raise_match_failure, {}}, partial, args,
                                               rows, [sc, n](const Partiality& p, const InitialPM& pm) {
                                                 return compile_match(sc, nullptr, p, context::start(n), pm);
                                               });
}

std::vector<const typedtree::Pattern*> flatten_pattern(long size, const typedtree::Pattern* p) {
  if (auto* t = as<tt::Tpat_tuple>(p->pat_desc)) {
    Pats ps;
    for (auto& x : t->pats) ps.push_back(x.pat);
    return ps;
  }
  if (p->pat_desc->kind == PK::Tpat_any) return omegas(size);
  throw CannotFlatten{};
}

// PR#4828: Believe it or not, the 'paraml' argument below may not be side
// effect free.
lam for_multiple_match(scopes sc, const Location& loc, Slice<lam> paraml, Slice<PatAction> pat_act_list,
                       typedtree::Partial partial) {
  // param_to_var, List.map: left to right
  std::vector<std::pair<Ident::t, lam>> v_paraml;
  for (lam param : paraml) {
    if (auto* v = L::as<L::Lvar>(param)) v_paraml.push_back({v->id, nullptr});
    else v_paraml.push_back({Ident::create_local("*match*"), param});
  }
  std::vector<Ident::t> vl;
  for (auto& [v, _] : v_paraml) vl.push_back(v);
  lam r = do_for_multiple_match(sc, loc, vl, pat_act_list, partial);
  for (auto it = v_paraml.rbegin(); it != v_paraml.rend(); ++it)
    if (it->second) r = L::bind(LetKind::Strict, it->first, it->second, r);
  return r;
}

lam for_optional_arg_default(scopes sc, const Location& loc, const typedtree::Pattern* pat, lam default_arg,
                             Ident::t param, lam body) {
  lam if_some = lprim(pfield(0, L::ImmediateOrPointer::Pointer, MutableFlag::Immutable), {L::lvar(param)},
                      loc_unknown());
  lam supplied_or_default = transl_match_on_option(L::lvar(param), loc_unknown(), if_some, default_arg);
  return for_let_(sc, loc, supplied_or_default, pat, body);
}

// Entry point of the string switch expansion (called by the bytecode
// compiler)
lam expand_stringswitch(const L::ScopedLocation& loc, lam arg, Slice<L::StringCase> sw0, lam d) {
  StrCases sw(sw0.begin(), sw0.end());
  if (!d) return bind_sw(arg, [&](lam a) { return do_make_string_test_tree(loc, a, sw, 0, nullptr); });
  return bind_sw(arg, [&](lam a) {
    return make_catch(d, [&](lam dd) { return do_make_string_test_tree(loc, a, sw, 1, dd); });
  });
}

lam inline_lazy_force(lam arg, const L::ScopedLocation& loc) { return inline_lazy_force_(arg, loc); }

}  // namespace cppcaml::typing::matching
