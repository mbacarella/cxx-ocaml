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
  int pos = -1;          // runtime field index; -1 when !runtime (see number())
  SigPtr sub;            // Module: its signature; Modtype: its body (may be
                         // null when unresolved)
  SigPtr functor_param;  // Module of functor type: parameter/result signatures
  SigPtr functor_result;
};

struct Sig {
  std::vector<Item> items;  // source order (shadowed items removed, see push)

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

}  // namespace cppcaml::modsig
