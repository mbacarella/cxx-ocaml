// The real ordered, namespaced module signature -- P1 substrate of the Env +
// Includemod-style coercion plan (JOURNAL "PLAN: REAL MODULE SYSTEM").
// Upstream pairs signature components by (namespace, name) and counts runtime
// positions over runtime components only (typing/includemod.ml,
// is_runtime_component: values except Val_prim, extension constructors,
// Mp_present modules and classes take a field; types, module types, class
// types, primitives and absent module aliases do not).  This is that data
// model for the C++ back end.  In P1 it is derived alongside the existing
// heuristic name-list layouts and asserted equal to them under
// CPPCAML_MODSIG_CHECK=1 -- no behavior change; P2 routes coercions through it.
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cppcaml::modsig {

// Signature-item namespace, as in includemod's FieldMap keys.  Unknown marks
// an item imported from a flat name-list (a layout not yet attributed to a
// namespace); it participates in runtime numbering like a value.
enum class NS : unsigned char {
  Value, Type, Typext, Module, Modtype, Class, ClassType, Unknown
};

struct Sig;
using SigPtr = std::shared_ptr<Sig>;

struct Item {
  NS ns = NS::Value;
  std::string name;
  bool runtime = true;   // occupies a field of the enclosing runtime block
  bool is_prim = false;  // declared `external` (upstream Val_prim: no field.
                         // The AST-signature layout path currently still gives
                         // it a slot; recorded so P2 can fix that deliberately)
  std::string prim;      // is_prim: the primitive descriptor ("%addint", C name)
  int prim_arity = 0;    // is_prim: number of arguments (eta-stub arity)
  int pos = -1;          // runtime field index; -1 when !runtime (see number())
  SigPtr sub;            // Module: its signature; Modtype: its body (may be
                         // null when unresolved)
  std::string alias_of;  // Module with !runtime: a signature alias
                         // `module M = P` (upstream Mp_absent -- no field);
                         // member reads substitute the target path P
  SigPtr functor_param;  // Module of functor type: parameter/result signatures
  SigPtr functor_result;
};

struct Sig {
  std::vector<Item> items;  // source order (shadowed items removed, see push)
  bool incomplete = false;  // some members may be MISSING entirely (an include
                            // of an unresolved module expression, a depth-guard
                            // truncation, a degraded empty flat import): a
                            // NEGATIVE member answer from such a Sig is not
                            // authoritative -- callers keep the flat fallback

  // Append with OCaml shadowing: an earlier same-namespace same-name item is
  // dropped, so the layout keeps only the LAST occurrence at its last
  // position (the cmi writer's dedup_shadowed_fields rule).  An Unknown item
  // (from a flat splice) shadows/is shadowed across namespaces -- that IS
  // today's flat-list semantics for includes -- but only among RUNTIME items
  // (the flat list holds runtime names only, so e.g. a `type map` must not
  // displace a spliced value `map`).  Once splices are namespaced (P2)
  // Unknown disappears and shadowing is purely per-namespace.
  void push(Item it) {
    for (std::size_t i = 0; i < items.size(); ++i)
      if (items[i].name == it.name &&
          (items[i].ns == it.ns ||
           ((items[i].ns == NS::Unknown || it.ns == NS::Unknown) &&
            items[i].runtime && it.runtime))) {
        items.erase(items.begin() + i);
        break;
      }
    items.push_back(std::move(it));
  }
  // Assign contiguous runtime positions in item order; call once construction
  // is complete (positions shift while shadowed items drop out).
  void number() {
    int p = 0;
    for (auto& it : items) it.pos = it.runtime ? p++ : -1;
  }
  int runtime_len() const {
    int n = 0;
    for (auto& it : items) n += it.runtime;
    return n;
  }
  std::vector<std::string> runtime_names() const {
    std::vector<std::string> v;
    for (auto& it : items)
      if (it.runtime) v.push_back(it.name);
    return v;
  }
  const Item* find(NS ns, const std::string& name) const {
    for (auto it = items.rbegin(); it != items.rend(); ++it)
      if (it->ns == ns && it->name == name) return &*it;
    return nullptr;
  }
};

// A flat imported layout (a name-list whose namespaces are unknown yet):
// runtime Unknown items in order.
inline SigPtr flat(const std::vector<std::string>& names) {
  auto s = std::make_shared<Sig>();
  for (auto& n : names) s->push({.ns = NS::Unknown, .name = n});
  s->number();
  // flat() is always a DEGRADED import; an empty one is indistinguishable
  // from "resolution failed", so its negative answers must not be trusted
  if (names.empty()) s->incomplete = true;
  return s;
}

// A scoped environment: one frame per structure/functor body under
// translation.  P1 binds modules and module types (each carrying its resolved
// Sig); values/types join in P3 when Env becomes the single source of path
// resolution.
struct Env {
  struct Frame {
    std::unordered_map<std::string, SigPtr> modules;
    std::unordered_map<std::string, SigPtr> modtypes;
  };
  std::vector<Frame> frames{{}};

  void push_frame() { frames.emplace_back(); }
  void pop_frame() {
    if (frames.size() > 1) frames.pop_back();
  }
  void bind_module(const std::string& n, SigPtr s) {
    frames.back().modules[n] = std::move(s);
  }
  void bind_modtype(const std::string& n, SigPtr s) {
    frames.back().modtypes[n] = std::move(s);
  }
  SigPtr lookup_module(const std::string& n) const {
    for (auto f = frames.rbegin(); f != frames.rend(); ++f)
      if (auto it = f->modules.find(n); it != f->modules.end())
        return it->second;
    return nullptr;
  }
  // The innermost binding for `n`, distinguishing "bound to null" (a local
  // module whose Sig is unknown -- a TOMBSTONE that must stop any fallback
  // resolution, e.g. to a shadowed cmi unit) from "not bound" (nullopt).
  std::optional<SigPtr> find_module(const std::string& n) const {
    for (auto f = frames.rbegin(); f != frames.rend(); ++f)
      if (auto it = f->modules.find(n); it != f->modules.end())
        return it->second;
    return std::nullopt;
  }
  SigPtr lookup_modtype(const std::string& n) const {
    for (auto f = frames.rbegin(); f != frames.rend(); ++f)
      if (auto it = f->modtypes.find(n); it != f->modtypes.end())
        return it->second;
    return nullptr;
  }
  // Resolve a dotted module path ("X.M.N"): scoped head lookup, then
  // navigation through nested Module items.
  SigPtr lookup_module_path(const std::string& dotted) const {
    std::size_t d = dotted.find('.');
    SigPtr cur = lookup_module(dotted.substr(0, d));
    while (cur && d != std::string::npos) {
      std::size_t e = dotted.find('.', d + 1);
      std::size_t len = e == std::string::npos ? std::string::npos : e - d - 1;
      const Item* m = cur->find(NS::Module, dotted.substr(d + 1, len));
      cur = m ? m->sub : nullptr;
      d = e;
    }
    return cur;
  }
};

// A COMPUTED module coercion over two namespaced Sigs -- the modsig analogue
// of typing/typedtree.mli's `module_coercion` (Tcoerce_*), replayed by
// lambda.cpp's apply_msig_coercion (mirrors lambda/translmod.ml apply_coercion).
// PURE DATA: names/indices/prim descriptors only; the replayer consults the
// Translator for the actual Lambda values.  One Field per TARGET runtime field,
// in target order.
struct Coercion;
using CoercionPtr = std::shared_ptr<Coercion>;
struct Coercion {
  struct Field {
    enum class From : unsigned char {
      SrcField,   // read src block field `src_pos` (Tcoerce_none/_structure)
      PrimStub,   // materialize an external as an eta-stub (Tcoerce_primitive)
      AliasValue, // materialize an elided module alias      (Tcoerce_alias)
    } from = From::SrcField;
    int src_pos = -1;          // SrcField: index in the SOURCE runtime block
    std::string name;          // the member name (all kinds; replay lookup key)
    std::string prim;          // PrimStub: primitive descriptor
    int prim_arity = 0;        // PrimStub: eta-stub arity
    CoercionPtr sub;           // recursive coercion for a submodule (else null)
    bool functor_eta = false;  // Tcoerce_functor: `sub` coerces the applied
                               // RESULT; replay eta-expands instead of
                               // projecting fields off the (closure) value
    CoercionPtr functor_arg;   // Tcoerce_functor cc_arg: coerces the wrapper's
                               // funarg (declared param layout) down to the
                               // implementation's parameter layout (else null)
  };
  std::vector<Field> fields;
  bool identity = false;  // src == tgt field-for-field AND same runtime length
  bool ok = true;         // false: a required member is absent/ambiguous
  std::string error;      // dotted path of the offending member when !ok
  int unknown_pairings = 0;  // # of fields paired via an Unknown namespace (a
                             // flat-splice escape hatch): observability for the
                             // trust gate (an all-known Sig pairs 0 this way).
};

// Locate the SOURCE item that satisfies a TARGET runtime item `t`, pairing by
// (namespace, name).  NS::Unknown on EITHER side pairs by name alone (the flat-
// splice escape hatch).  Last occurrence wins (shadowing already applied by
// push()).  `via_unknown` is set when the pairing crossed an Unknown namespace.
inline const Item* coercion_find_src(const Sig& src, const Item& t,
                                     bool& via_unknown) {
  via_unknown = false;
  const Item* best = nullptr;
  for (auto it = src.items.rbegin(); it != src.items.rend(); ++it) {
    if (it->name != t.name) continue;
    bool exact = (it->ns == t.ns);
    bool unknown = (it->ns == NS::Unknown || t.ns == NS::Unknown);
    if (exact) { via_unknown = false; return &*it; }
    if (unknown && !best) { best = &*it; via_unknown = true; }
  }
  return best;
}

inline bool trusted(const Sig& s, int depth);

// A Sig is layout-complete when neither it nor any nested module/functor Sig
// is marked incomplete.  An incomplete Sig may be MISSING members entirely
// (an unresolved modtype degrades to an empty flat, which is also vacuously
// trusted), so a coercion computed against it could silently drop fields.
inline bool layout_complete(const Sig& s, int depth = 0) {
  if (s.incomplete || depth > 24) return false;
  for (auto& it : s.items) {
    if (it.sub && !layout_complete(*it.sub, depth + 1)) return false;
    if (it.functor_param && !layout_complete(*it.functor_param, depth + 1))
      return false;
    if (it.functor_result && !layout_complete(*it.functor_result, depth + 1))
      return false;
  }
  return true;
}

// A coercion replayable as pure field selection: every field reads a source
// field, recursing into submodule sub-coercions.  Prim stubs, alias
// materialization and nested functor etas need translator state at the replay
// site, so an ARGUMENT coercion (replayed on a bare funarg) is only carried
// when it avoids them.
inline bool pure_projection(const Coercion& c, int depth = 0) {
  if (depth > 24) return false;
  for (auto& f : c.fields) {
    if (f.from != Coercion::Field::From::SrcField || f.functor_eta)
      return false;
    if (f.sub && !pure_projection(*f.sub, depth + 1)) return false;
  }
  return true;
}

// Compute the coercion that builds `tgt`'s runtime block from `src`'s, matching
// includemod.signatures.  Non-runtime target items (types, modtypes, class
// types, primitives, absent aliases) take no field and are skipped WITHOUT a
// src presence check -- the typer owns rejection; the back end owns layout.
inline Coercion compute_coercion(const Sig& src, const Sig& tgt, int depth = 0) {
  Coercion c;
  if (depth > 24) { c.identity = true; return c; }  // recursive modtype guard
  for (const Item& t : tgt.items) {
    if (!t.runtime) continue;  // no field: skip (see note above)
    bool via_unknown = false;
    const Item* s = coercion_find_src(src, t, via_unknown);
    if (!s) { c.ok = false; c.error = t.name; return c; }
    if (via_unknown) ++c.unknown_pairings;
    Coercion::Field f;
    f.name = t.name;
    if (s->runtime) {
      f.from = Coercion::Field::From::SrcField;
      f.src_pos = s->pos;
      if (s->ns == NS::Module && t.ns == NS::Module && s->sub && t.sub) {
        Coercion sub = compute_coercion(*s->sub, *t.sub, depth + 1);
        if (!sub.ok) { c.ok = false; c.error = t.name + "." + sub.error; return c; }
        c.unknown_pairings += sub.unknown_pairings;
        if (!sub.identity) f.sub = std::make_shared<Coercion>(std::move(sub));
      } else if (s->ns == NS::Module && t.ns == NS::Module &&
                 s->functor_result && t.functor_result &&
                 layout_complete(*s->functor_result) &&
                 layout_complete(*t.functor_result)) {
        // Tcoerce_functor (translmod.apply_coercion): the eta wrapper passes
        // the parameter through unchanged and coerces the applied RESULT.
        // Passing a restricted functor through raw would let a cmi-driven
        // consumer read the raw result at the restricted layout's offsets.
        // Layout-incomplete result Sigs (an unresolved local modtype) fall
        // through to the raw pass-through instead of a wrong projection.
        CoercionPtr ac;
        if (s->functor_param && t.functor_param) {
          // contravariant: the wrapper's funarg arrives at the DECLARED param
          // layout and the raw functor reads the implementation's -- cc_arg
          // builds the impl-layout block from the declared-layout funarg.
          Coercion pc = compute_coercion(*t.functor_param, *s->functor_param,
                                         depth + 1);
          if (!pc.ok) { c.ok = false; c.error = t.name + "(param)"; return c; }
          if (!pc.identity) {
            // carried only as a pure projection over complete, trusted param
            // Sigs; anything else keeps the previous decline (legacy path)
            if (!layout_complete(*s->functor_param) ||
                !layout_complete(*t.functor_param) ||
                !trusted(*s->functor_param, depth + 1) ||
                !trusted(*t.functor_param, depth + 1) ||
                !pure_projection(pc)) {
              c.ok = false; c.error = t.name + "(param)"; return c;
            }
            c.unknown_pairings += pc.unknown_pairings;
            ac = std::make_shared<Coercion>(std::move(pc));
          }
        }
        Coercion rc = compute_coercion(*s->functor_result, *t.functor_result,
                                       depth + 1);
        if (!rc.ok) {
          c.ok = false; c.error = t.name + "()." + rc.error; return c;
        }
        c.unknown_pairings += rc.unknown_pairings;
        if (!rc.identity || ac) {
          if (!rc.identity &&
              (!trusted(*s->functor_result, depth + 1) ||
               !trusted(*t.functor_result, depth + 1))) {
            c.ok = false; c.error = t.name + "(untrusted)"; return c;
          }
          f.functor_eta = true;
          f.sub = std::make_shared<Coercion>(std::move(rc));
          f.functor_arg = std::move(ac);
        }
      }
    } else if (s->is_prim) {  // external (no slot) exposed as a val: eta-stub it
      f.from = Coercion::Field::From::PrimStub;
      f.prim = s->prim;
      f.prim_arity = s->prim_arity;
    } else if (s->ns == NS::Module) {  // elided alias exposed: materialize it
      f.from = Coercion::Field::From::AliasValue;
    } else {  // a no-slot src member that is neither prim nor alias: unmatchable
      c.ok = false; c.error = t.name; return c;
    }
    c.fields.push_back(std::move(f));
  }
  // Identity per simplify_structure_coercion + PR#5098: fields are 0,1,2,..
  // with no subs AND the source has no EXTRA runtime fields.
  c.identity = ((int)c.fields.size() == src.runtime_len());
  for (std::size_t i = 0; i < c.fields.size() && c.identity; ++i)
    if (c.fields[i].from != Coercion::Field::From::SrcField ||
        c.fields[i].src_pos != (int)i || c.fields[i].sub)
      c.identity = false;
  return c;
}

// A computed coercion is TRUSTED only when derived from fully-namespaced Sigs
// (no Unknown items, recursively) -- the migration safety valve.  An empty Sig
// is trusted-but-useless; the call site treats it as untrusted.
inline bool trusted(const Sig& s, int depth = 0) {
  if (depth > 24) return false;
  for (auto& it : s.items) {
    if (it.ns == NS::Unknown) return false;
    if (it.sub && !trusted(*it.sub, depth + 1)) return false;
  }
  return true;
}

}  // namespace cppcaml::modsig
