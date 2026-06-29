#include "cppcaml/typer.hpp"

#include <unordered_map>
#include <unordered_set>

#include "cppcaml/cmi.hpp"
#include "cppcaml/infer_check.hpp"

namespace cppcaml {
namespace {

namespace tt = typedtree;
using namespace ast;

// The set of value names the implicit `open Stdlib` brings into scope, loaded
// once from stdlib.cmi (relative to the repo root, where the harness runs).
const std::unordered_set<std::string>& stdlib_values() {
  static const std::unordered_set<std::string> s = [] {
    std::unordered_set<std::string> out;
    try {
      auto cmi = cmi::CmiFile::load("stdlib/stdlib.cmi");
      for (auto& v : cmi.values()) out.insert(v.name);
    } catch (...) {
      // No stdlib found: only locals will resolve.
    }
    return out;
  }();
  return s;
}

// Predefined type constructors (Predef idents, printed name/stamp!).  Stamp
// values are arbitrary post-normalization; only distinctness matters.
const std::unordered_map<std::string, long long>& predef_types() {
  static const std::unordered_map<std::string, long long> m = {
      {"int", 1},      {"char", 2},    {"bytes", 3},   {"float", 4},
      {"bool", 5},     {"unit", 6},    {"exn", 7},     {"array", 8},
      {"list", 9},     {"option", 10}, {"nativeint", 11}, {"int32", 12},
      {"int64", 13},   {"lazy_t", 14}, {"string", 17}, {"floatarray", 16},
      {"extension_constructor", 15},
  };
  return m;
}

// ---- format-string desugaring -------------------------------------------
// DRAFT (written 2026-06-05, NOT yet built/verified — pending machine recovery).
// A string literal in a format-typed position becomes
//   CamlinternalFormatBasics.Format(fmt_tree, original_string)
// where fmt_tree is a chain of CamlinternalFormatBasics constructors, all ghost
// at the string's location.  Structure confirmed from `ocamlc -dtypedtree` on
// `Printf.printf "%d\n" 1`.  Conservative: covers literal runs + the common
// plain directives (No_padding/No_precision); ANYTHING with flags/width/
// precision (e.g. %5d, %.3f, %ld, %a) makes `make` return null so the caller
// leaves the argument as a plain string (the file stays a safe DIFF, no
// regression).  TODO before relying on it: verify against the oracle, then add
// padding/precision/%a/%t and unqualified (open Printf) detection.
namespace fmtlib {
inline std::string ns(const std::string& n) { return "CamlinternalFormatBasics." + n; }

inline tt::ExprBox gctor(const std::string& n, std::vector<tt::ExprBox> args,
                         const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  e->desc = tt::Texp_construct{ns(n), std::move(args)};
  return e;
}
inline tt::ExprBox gchar(unsigned char c, const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  ast::Constant k;
  k.loc = g;
  ast::Pconst_char pc;
  pc.code = c;
  k.desc = pc;
  e->desc = tt::Texp_constant{std::move(k)};
  return e;
}
inline tt::ExprBox gstr(const std::string& s, const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  ast::Constant k;
  k.loc = g;
  ast::Pconst_string ps;
  ps.s = s;
  ps.strloc = g;
  k.desc = ps;
  e->desc = tt::Texp_constant{std::move(k)};
  return e;
}

// One plain directive `%<d>`; ok=false (and returns rest unchanged) for anything
// not handled, so the caller bails out of converting this format.
inline tt::ExprBox directive(char d, tt::ExprBox rest, const Location& g, bool& ok) {
  ok = true;
  auto intc = [&](const char* conv) {
    std::vector<tt::ExprBox> a;
    a.push_back(gctor(conv, {}, g));
    a.push_back(gctor("No_padding", {}, g));
    a.push_back(gctor("No_precision", {}, g));
    a.push_back(std::move(rest));
    return gctor("Int", std::move(a), g);
  };
  switch (d) {
    case 'd': return intc("Int_d");
    case 'i': return intc("Int_i");
    case 'u': return intc("Int_u");
    case 'x': return intc("Int_x");
    case 'X': return intc("Int_X");
    case 'o': return intc("Int_o");
    case 's': {
      std::vector<tt::ExprBox> a;
      a.push_back(gctor("No_padding", {}, g));
      a.push_back(std::move(rest));
      return gctor("String", std::move(a), g);
    }
    case 'c': {
      std::vector<tt::ExprBox> a;
      a.push_back(std::move(rest));
      return gctor("Char", std::move(a), g);
    }
    case 'b': {
      std::vector<tt::ExprBox> a;
      a.push_back(gctor("No_padding", {}, g));
      a.push_back(std::move(rest));
      return gctor("Bool", std::move(a), g);
    }
    case 'f': case 'e': case 'g': case 'E': case 'F': {
      const char* fc = d == 'f' ? "Float_f" : d == 'e' ? "Float_e"
                     : d == 'g' ? "Float_g" : d == 'E' ? "Float_E" : "Float_F";
      std::vector<tt::ExprBox> a;
      a.push_back(gctor(fc, {}, g));
      a.push_back(gctor("No_padding", {}, g));
      a.push_back(gctor("No_precision", {}, g));
      a.push_back(std::move(rest));
      return gctor("Float", std::move(a), g);
    }
    default: ok = false; return rest;
  }
}

// Parse s[i..] into the fmt tree; null if it contains anything unsupported.
inline tt::ExprBox parse(const std::string& s, size_t i, const Location& g) {
  if (i >= s.size()) return gctor("End_of_format", {}, g);
  if (s[i] == '%') {
    if (i + 1 >= s.size()) return nullptr;
    char d = s[i + 1];
    if (d == '%') {
      auto r = parse(s, i + 2, g);
      if (!r) return nullptr;
      return gctor("Char_literal", [&] { std::vector<tt::ExprBox> a; a.push_back(gchar('%', g)); a.push_back(std::move(r)); return a; }(), g);
    }
    if (d == '!') {
      auto r = parse(s, i + 2, g);
      if (!r) return nullptr;
      std::vector<tt::ExprBox> a; a.push_back(std::move(r));
      return gctor("Flush", std::move(a), g);
    }
    auto r = parse(s, i + 2, g);
    if (!r) return nullptr;
    bool ok;
    auto e = directive(d, std::move(r), g, ok);
    return ok ? std::move(e) : nullptr;
  }
  size_t k = i;
  while (k < s.size() && s[k] != '%') ++k;
  auto r = parse(s, k, g);
  if (!r) return nullptr;
  std::vector<tt::ExprBox> a;
  if (k - i == 1) {
    a.push_back(gchar(static_cast<unsigned char>(s[i]), g));
    a.push_back(std::move(r));
    return gctor("Char_literal", std::move(a), g);
  }
  a.push_back(gstr(s.substr(i, k - i), g));
  a.push_back(std::move(r));
  return gctor("String_literal", std::move(a), g);
}

// Build the CamlinternalFormatBasics.Format(...) wrapper, or null if the format
// uses anything we don't yet desugar.
inline tt::ExprBox make(const std::string& s, const Location& sloc) {
  Location g = sloc;
  g.ghost = true;
  auto tree = parse(s, 0, g);
  if (!tree) return nullptr;
  std::vector<tt::ExprBox> a;
  a.push_back(std::move(tree));
  a.push_back(gstr(s, g));
  return gctor("Format", std::move(a), sloc);  // outer wrapper keeps the real loc
}
}  // namespace fmtlib

// Build the Stdlib path Stdlib!.name (Pdot over a global Stdlib ident).
tt::Path stdlib_path(const std::string& name) {
  auto pre = std::make_shared<tt::Path>();
  pre->v = tt::Pident{tt::Ident{"Stdlib", 0, tt::Ident::Global}};
  tt::Path p;
  p.v = tt::Pdot{pre, name};
  return p;
}

struct Typer {
  // Side-table from the inference pass: match nodes that are non-exhaustive.
  const std::unordered_map<const ast::Expression*, bool>* partiality = nullptr;
  long long next_stamp = 274;  // arbitrary base; the harness normalizes stamps
  // Scope frames mapping value name -> local ident; innermost last.
  std::vector<std::unordered_map<std::string, tt::Ident>> scopes{{}};

  // Type constructors, submodules, and module types (own namespaces).
  std::unordered_map<std::string, tt::Ident> type_scope;
  std::unordered_map<std::string, tt::Ident> module_scope;
  std::unordered_map<std::string, tt::Ident> modtype_scope;

  // Record fields: name -> the record type's full field list (decl order) +
  // representation.  Drives Texp_record's decl-order emission and <kept> fields.
  struct RecordInfo {
    std::vector<std::string> decl_fields;
    std::string repr = "Record_regular";
  };
  std::unordered_map<std::string, RecordInfo> field_registry;
  // Local type names whose manifest resolves to `float` (`type t = [private]
  // float`, transitively): a record all of whose fields have such a type gets
  // the Record_float representation, like a bare-`float` record.
  std::set<std::string> float_abbrevs_;

  static std::string lid_last(const Longident& x) {
    if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
    if (auto* p = std::get_if<Ldot>(&x.v)) return p->name;
    return "?";
  }

  // Index of the format-string argument for a qualified Printf/Format/Scanf
  // call (draft: qualified only, to avoid mis-converting shadowed names), or -1.
  static int format_arg_index(const Longident& fn) {
    auto* d = std::get_if<Ldot>(&fn.v);
    if (!d) return -1;
    std::string mod = lid_last(*d->prefix);
    if (mod != "Printf" && mod != "Format" && mod != "Scanf") return -1;
    const std::string& n = d->name;
    if (n == "printf" || n == "eprintf" || n == "sprintf" || n == "asprintf" ||
        n == "dprintf" || n == "scanf")
      return 0;
    if (n == "fprintf" || n == "ifprintf" || n == "bprintf" || n == "ksprintf" ||
        n == "kprintf" || n == "kasprintf" || n == "sscanf" || n == "bscanf")
      return 1;
    if (n == "kfprintf" || n == "kbprintf") return 2;
    return -1;
  }

  // Module-level `open M`: names exported by M resolve through M's path.
  struct OpenEntry {
    tt::Path path;
    std::unordered_set<std::string> values;
    std::unordered_set<std::string> types;
  };
  std::vector<OpenEntry> opens;

  void load_open_names(const std::string& modname, OpenEntry& oe) {
    try {
      auto cmi = cmi::CmiFile::load("stdlib/stdlib__" + modname + ".cmi");
      for (auto& v : cmi.values()) oe.values.insert(v.name);
      for (auto& t : cmi.types()) oe.types.insert(t.name);
    } catch (...) {
      // Unknown/local module: names from it won't resolve (best effort).
    }
  }

  tt::Ident fresh_local(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    scopes.back()[name] = id;
    return id;
  }
  tt::Ident fresh_anon(const std::string& name) {  // stamped, not scoped
    return tt::Ident{name, next_stamp++, tt::Ident::Local};
  }
  tt::Ident fresh_type(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    type_scope[name] = id;
    return id;
  }
  tt::Ident fresh_module(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    module_scope[name] = id;
    return id;
  }
  tt::Ident fresh_modtype(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    modtype_scope[name] = id;
    return id;
  }
  void push() { scopes.emplace_back(); }
  void pop() { scopes.pop_back(); }

  // Resolve a type constructor: local type, else predef, else qualified path.
  tt::Path resolve_type(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto t = type_scope.find(l->name);
      if (t != type_scope.end()) {
        tt::Path p;
        p.v = tt::Pident{t->second};
        return p;
      }
      auto pd = predef_types().find(l->name);
      if (pd != predef_types().end()) {
        tt::Path p;
        p.v = tt::Pident{tt::Ident{l->name, pd->second, tt::Ident::Predef}};
        return p;
      }
      for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
        if (it->types.count(l->name)) {
          tt::Path p;
          p.v = tt::Pdot{std::make_shared<tt::Path>(it->path), l->name};
          return p;
        }
      }
      throw TypeError("Unbound type constructor " + l->name);
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported type path");
  }

  tt::CoreType core_type(const CoreType& t) {
    tt::CoreType out;
    out.loc = t.loc;
    if (std::holds_alternative<Ptyp_any>(t.desc)) {
      out.desc = tt::Ttyp_any{};
    } else if (auto* v = std::get_if<Ptyp_var>(&t.desc)) {
      out.desc = tt::Ttyp_var{v->name};
    } else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
      out.desc = tt::Ttyp_arrow{a->label,
                                std::make_unique<tt::CoreType>(core_type(*a->dom)),
                                std::make_unique<tt::CoreType>(core_type(*a->cod))};
    } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      tt::Ttyp_tuple tup;
      for (size_t k = 0; k < tu->elems.size(); ++k) {
        std::optional<std::string> label;
        if (k < tu->labels.size()) label = tu->labels[k];
        tup.elems.emplace_back(
            label, std::make_unique<tt::CoreType>(core_type(*tu->elems[k])));
      }
      out.desc = std::move(tup);
    } else if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      tt::Ttyp_constr tc;
      tc.path = resolve_type(c->id.txt);
      for (auto& arg : c->args)
        tc.args.push_back(std::make_unique<tt::CoreType>(core_type(*arg)));
      out.desc = std::move(tc);
    } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
      out.desc = tt::Ttyp_alias{al->name,
                                std::make_unique<tt::CoreType>(core_type(*al->type))};
    } else if (auto* po = std::get_if<Ptyp_poly>(&t.desc)) {
      out.desc = tt::Ttyp_poly{po->vars,
                               std::make_unique<tt::CoreType>(core_type(*po->type))};
    } else {
      throw TypeError("coretype#" + std::to_string(t.desc.index()));
    }
    return out;
  }

  // Record fields wrap their type in Ttyp_poly([], inner).
  tt::CoreType poly_wrap(const CoreType& t) {
    tt::CoreType inner = core_type(t);
    tt::CoreType poly;
    poly.loc = inner.loc;
    poly.desc = tt::Ttyp_poly{{}, std::make_unique<tt::CoreType>(std::move(inner))};
    return poly;
  }

  std::vector<tt::CoreTypeBox> ctor_args(const ConstructorArguments& a) {
    std::vector<tt::CoreTypeBox> out;
    if (auto* t = std::get_if<Pcstr_tuple>(&a)) {
      for (auto& el : t->elems)
        out.push_back(std::make_unique<tt::CoreType>(core_type(*el)));
    } else {
      throw TypeError("inline record constructor");
    }
    return out;
  }

  tt::ExtCtor ext_ctor(const ExtensionConstructor& c) {
    auto* decl = std::get_if<Pext_decl>(&c.kind);
    if (!decl) throw TypeError("extension rebind");
    tt::ExtCtor out;
    out.loc = c.loc;
    out.id = fresh_anon(c.name.txt);
    out.args = ctor_args(decl->args);
    if (decl->res) out.res = std::make_unique<tt::CoreType>(core_type(**decl->res));
    return out;
  }

  tt::ConstructorDecl constructor_decl(const ConstructorDecl& c) {
    tt::ConstructorDecl out;
    out.loc = c.loc;
    out.id = fresh_anon(c.name.txt);
    out.args = ctor_args(c.args);
    if (c.res) out.res = std::make_unique<tt::CoreType>(core_type(**c.res));
    return out;
  }

  tt::LabelDecl label_decl(const LabelDecl& f) {
    tt::LabelDecl out;
    out.loc = f.loc;
    out.mutable_ = f.mut == MutableFlag::Mutable;
    out.id = fresh_anon(f.name.txt);
    out.type = poly_wrap(*f.type);
    return out;
  }

  tt::TypeKind type_kind(const TypeKind& k) {
    tt::TypeKind out;
    if (std::holds_alternative<Ptype_abstract>(k)) {
      out.v = tt::Ttype_abstract{};
    } else if (auto* v = std::get_if<Ptype_variant>(&k)) {
      tt::Ttype_variant tv;
      for (auto& c : v->ctors) tv.ctors.push_back(constructor_decl(c));
      out.v = std::move(tv);
    } else if (auto* r = std::get_if<Ptype_record>(&k)) {
      tt::Ttype_record tr;
      for (auto& f : r->fields) tr.labels.push_back(label_decl(f));
      out.v = std::move(tr);
    } else if (std::holds_alternative<Ptype_open>(k)) {
      out.v = tt::Ttype_open{};
    } else {
      throw TypeError("type_external kind");
    }
    return out;
  }

  tt::TypeDeclaration type_declaration(const TypeDeclaration& d) {
    tt::TypeDeclaration td;
    td.id = type_scope.at(d.name.txt);
    td.loc = d.loc;
    td.attrs = &d.attrs;
    for (auto& p : d.params)
      td.params.push_back(std::make_unique<tt::CoreType>(core_type(*p)));
    td.kind = type_kind(d.kind);
    td.private_ = d.priv == PrivateFlag::Private;
    if (d.manifest)
      td.manifest = std::make_unique<tt::CoreType>(core_type(**d.manifest));
    return td;
  }

  // Transcribe a (recursive) type-declaration group: pre-bind names, register
  // record fields, then transcribe bodies.  Shared by Pstr_type and Psig_type.
  std::vector<tt::TypeDeclaration> type_decls(
      const std::vector<TypeDeclaration>& decls) {
    for (auto& d : decls) fresh_type(d.name.txt);
    // Register float abbreviations first (`type t = [private] float`, or an alias
    // of an already-known float type), so a same-group float record sees them.
    auto is_float_ty = [&](const CoreType& t) {
      auto* c = std::get_if<Ptyp_constr>(&t.desc);
      return c && (lid_last(c->id.txt) == "float" || float_abbrevs_.count(lid_last(c->id.txt)));
    };
    for (auto& d : decls)
      if (d.manifest && is_float_ty(*d.manifest->get())) float_abbrevs_.insert(d.name.txt);
    for (auto& d : decls) {
      if (auto* r = std::get_if<Ptype_record>(&d.kind)) {
        RecordInfo info;
        bool all_float = !r->fields.empty();
        for (auto& f : r->fields) {
          info.decl_fields.push_back(f.name.txt);
          if (!is_float_ty(*f.type)) all_float = false;
        }
        bool unboxed = false;
        for (auto& a : d.attrs)
          if (a.name == "unboxed" || a.name == "ocaml.unboxed") unboxed = true;
        if (all_float) info.repr = "Record_float";
        else if (unboxed) info.repr = "Record_unboxed false";
        for (auto& f : r->fields) field_registry[f.name.txt] = info;
      }
    }
    std::vector<tt::TypeDeclaration> out;
    for (auto& d : decls) out.push_back(type_declaration(d));
    return out;
  }

  // Resolve an unqualified value name: locals (innermost first), else Stdlib.
  tt::Path resolve_value(const Longident& lid, size_t err_pos) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto f = it->find(l->name);
        if (f != it->end()) {
          tt::Path p;
          p.v = tt::Pident{f->second};
          return p;
        }
      }
      for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
        if (it->values.count(l->name)) {
          tt::Path p;
          p.v = tt::Pdot{std::make_shared<tt::Path>(it->path), l->name};
          return p;
        }
      }
      if (stdlib_values().count(l->name)) return stdlib_path(l->name);
      throw TypeError("Unbound value " + l->name);
    }
    // Qualified M.x: resolve through Stdlib's implicit open for the head, then
    // append the field path.  (Only the common M.x shape for now.)
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported longident");
  }

  // Resolve a module path head: local submodule, else Stdlib (implicit open).
  tt::Path resolve_module(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto m = module_scope.find(l->name);
      if (m != module_scope.end()) {
        tt::Path p;
        p.v = tt::Pident{m->second};
        return p;
      }
      auto pre = std::make_shared<tt::Path>();
      pre->v = tt::Pident{tt::Ident{"Stdlib", 0, tt::Ident::Global}};
      tt::Path p;
      p.v = tt::Pdot{pre, l->name};
      return p;
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported module path");
  }

  // Resolve a module-type path: local module type, else qualified.
  tt::Path resolve_modtype(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto m = modtype_scope.find(l->name);
      if (m != modtype_scope.end()) {
        tt::Path p;
        p.v = tt::Pident{m->second};
        return p;
      }
      throw TypeError("Unbound module type " + l->name);
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported module-type path");
  }

  static std::string lid_str(const Longident& x) {
    if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
    if (auto* p = std::get_if<Ldot>(&x.v)) return lid_str(*p->prefix) + '.' + p->name;
    auto& a = std::get<Lapply>(x.v);
    return lid_str(*a.f) + '(' + lid_str(*a.x) + ')';
  }

  // A constructor of arity>1 applied to a literal tuple flattens its arguments
  // in the typedtree.  Only `::` is arity-2 among the constructors we resolve
  // without Env; everything else keeps a single (possibly tuple) argument.
  static bool flattens(const std::string& name) { return name == "::"; }

  tt::Pattern pattern(const Pattern& p) {
    tt::Pattern out;
    out.loc = p.loc;
    out.attrs = &p.attrs;
    if (std::holds_alternative<Ppat_any>(p.desc)) {
      out.desc = tt::Tpat_any{};
    } else if (auto* v = std::get_if<Ppat_var>(&p.desc)) {
      out.desc = tt::Tpat_var{fresh_local(v->name.txt)};
    } else if (auto* c = std::get_if<Ppat_constant>(&p.desc)) {
      out.desc = tt::Tpat_constant{c->c};
    } else if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
      tt::Tpat_construct tc;
      tc.name = lid_str(k->id.txt);
      if (k->arg) {
        auto* tup = std::get_if<Ppat_tuple>(&(*k->arg)->desc);
        if (tup && flattens(tc.name)) {
          for (auto& el : tup->elems)
            tc.args.push_back(std::make_unique<tt::Pattern>(pattern(*el)));
        } else {
          tc.args.push_back(std::make_unique<tt::Pattern>(pattern(**k->arg)));
        }
      }
      out.desc = std::move(tc);
    } else if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
      tt::Tpat_tuple tup;
      for (auto& el : tu->elems)
        tup.elems.emplace_back(std::nullopt,
                               std::make_unique<tt::Pattern>(pattern(*el)));
      out.desc = std::move(tup);
    } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      out.desc = tt::Tpat_or{std::make_unique<tt::Pattern>(pattern(*o->l)),
                             std::make_unique<tt::Pattern>(pattern(*o->r))};
    } else if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      auto inner = std::make_unique<tt::Pattern>(pattern(*al->p));
      tt::Tpat_alias ta;
      ta.id = fresh_local(al->name.txt);
      ta.inner = std::move(inner);
      out.desc = std::move(ta);
    } else if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) {
      out = pattern(*ct->p);  // become inner pattern; record constraint as extra
      out.extras.push_back(tt::PatExtra{core_type(*ct->t), p.loc});
    } else if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      tt::Tpat_record tr;  // only the written fields, in order (label = last comp)
      for (auto& [lid, sub] : r->fields)
        tr.fields.emplace_back(lid_last(lid.txt),
                               std::make_unique<tt::Pattern>(pattern(*sub)));
      out.desc = std::move(tr);
    } else if (auto* a = std::get_if<Ppat_array>(&p.desc)) {
      tt::Tpat_array ta;
      for (auto& el : a->elems) ta.elems.push_back(std::make_unique<tt::Pattern>(pattern(*el)));
      out.desc = std::move(ta);
    } else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) {
      out.desc = tt::Tpat_lazy{std::make_unique<tt::Pattern>(pattern(*lz->p))};
    } else {
      throw TypeError("pat#" + std::to_string(p.desc.index()));
    }
    return out;
  }

  // Build a computation pattern (match case lhs): or distributes, `exception P`
  // becomes Tpat_exception, and any other (value) pattern is wrapped Tpat_value.
  tt::Pattern to_computation(const Pattern& p) {
    tt::Pattern out;
    out.loc = p.loc;
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      out.desc = tt::Tpat_or{std::make_unique<tt::Pattern>(to_computation(*o->l)),
                             std::make_unique<tt::Pattern>(to_computation(*o->r))};
    } else if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) {
      out.desc = tt::Tpat_exception{std::make_unique<tt::Pattern>(pattern(*ex->p))};
    } else {
      tt::Pattern inner = pattern(p);
      out.loc = inner.loc;
      out.desc = tt::Tpat_value{std::make_unique<tt::Pattern>(std::move(inner))};
    }
    return out;
  }

  tt::Expression expr(const Expression& e) {
    tt::Expression out;
    out.loc = e.loc;
    out.attrs = &e.attrs;
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) {
      out.desc = tt::Texp_constant{c->c};
    } else if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      out.desc = tt::Texp_ident{resolve_value(id->id.txt, e.loc.start.cnum)};
    } else if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      tt::Texp_tuple tup;
      for (size_t k = 0; k < t->elems.size(); ++k) {
        std::optional<std::string> label;
        if (k < t->labels.size()) label = t->labels[k];
        tup.elems.emplace_back(
            label, std::make_unique<tt::Expression>(expr(*t->elems[k])));
      }
      out.desc = std::move(tup);
    } else if (auto* a = std::get_if<Pexp_apply>(&e.desc)) {
      tt::Texp_apply ap;
      ap.fn = std::make_unique<tt::Expression>(expr(*a->fn));
      // Detect a format-string argument to a qualified printf/scanf-family call
      // and desugar it to CamlinternalFormatBasics.Format(...).
      int fmtidx = -1;
      if (auto* fid = std::get_if<Pexp_ident>(&a->fn->desc))
        fmtidx = format_arg_index(fid->id.txt);
      for (size_t k = 0; k < a->args.size(); ++k) {
        auto& [label, arg] = a->args[k];
        tt::ExprBox av;
        if (static_cast<int>(k) == fmtidx)
          if (auto* cst = std::get_if<Pexp_constant>(&arg->desc))
            if (auto* ps = std::get_if<ast::Pconst_string>(&cst->c.desc))
              av = fmtlib::make(ps->s, arg->loc);
        if (!av) av = std::make_unique<tt::Expression>(expr(*arg));
        ap.args.emplace_back(label, std::move(av));
      }
      out.desc = std::move(ap);
    } else if (auto* f = std::get_if<Pexp_function>(&e.desc)) {
      out.desc = function(*f);
    } else if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      tt::Texp_let tl;
      tl.rf = le->rf;
      push();
      tl.bindings = value_bindings(le->rf, le->bindings);
      tl.body = std::make_unique<tt::Expression>(expr(*le->body));
      pop();
      out.desc = std::move(tl);
    } else if (auto* it = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      tt::Texp_ifthenelse ti;
      ti.cond = std::make_unique<tt::Expression>(expr(*it->cond));
      ti.then_ = std::make_unique<tt::Expression>(expr(*it->then_));
      if (it->else_)
        ti.else_ = std::make_unique<tt::Expression>(expr(**it->else_));
      out.desc = std::move(ti);
    } else if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) {
      tt::Texp_sequence ts;
      ts.e1 = std::make_unique<tt::Expression>(expr(*s->e1));
      ts.e2 = std::make_unique<tt::Expression>(expr(*s->e2));
      out.desc = std::move(ts);
    } else if (auto* m = std::get_if<Pexp_match>(&e.desc)) {
      tt::Texp_match tm;
      tm.scrut = std::make_unique<tt::Expression>(expr(*m->e));
      for (auto& c : m->cases) tm.cases.push_back(case_(c, /*computation=*/true));
      if (partiality) {
        auto it = partiality->find(&e);
        tm.partial = it != partiality->end() && it->second;
      }
      out.desc = std::move(tm);
    } else if (auto* tr = std::get_if<Pexp_try>(&e.desc)) {
      tt::Texp_try tt2;
      tt2.body = std::make_unique<tt::Expression>(expr(*tr->e));
      for (auto& c : tr->cases) tt2.cases.push_back(case_(c, /*computation=*/false));
      out.desc = std::move(tt2);
    } else if (auto* k = std::get_if<Pexp_construct>(&e.desc)) {
      tt::Texp_construct tc;
      tc.name = lid_str(k->id.txt);
      if (k->arg) {
        auto* tup = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
        if (tup && flattens(tc.name)) {
          for (auto& el : tup->elems)
            tc.args.push_back(std::make_unique<tt::Expression>(expr(*el)));
        } else {
          tc.args.push_back(std::make_unique<tt::Expression>(expr(**k->arg)));
        }
      }
      out.desc = std::move(tc);
    } else if (auto* ar = std::get_if<Pexp_array>(&e.desc)) {
      tt::Texp_array ta;
      for (auto& el : ar->elems)
        ta.elems.push_back(std::make_unique<tt::Expression>(expr(*el)));
      out.desc = std::move(ta);
    } else if (auto* as = std::get_if<Pexp_assert>(&e.desc)) {
      out.desc = tt::Texp_assert{std::make_unique<tt::Expression>(expr(*as->e))};
    } else if (auto* fo = std::get_if<Pexp_for>(&e.desc)) {
      tt::Texp_for tf;
      tf.lo = std::make_unique<tt::Expression>(expr(*fo->lo));
      tf.hi = std::make_unique<tt::Expression>(expr(*fo->hi));
      tf.dir = fo->dir == DirectionFlag::Upto ? tt::Direction::Up
                                              : tt::Direction::Down;
      push();
      auto* pv = std::get_if<Ppat_var>(&fo->var.desc);
      if (!pv) { pop(); throw TypeError("for-var not a variable"); }
      tf.var = fresh_local(pv->name.txt);
      tf.body = std::make_unique<tt::Expression>(expr(*fo->body));
      pop();
      out.desc = std::move(tf);
    } else if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) {
      out.desc = tt::Texp_lazy{std::make_unique<tt::Expression>(expr(*lz->e))};
    } else if (auto* wh = std::get_if<Pexp_while>(&e.desc)) {
      out.desc = tt::Texp_while{std::make_unique<tt::Expression>(expr(*wh->cond)),
                                std::make_unique<tt::Expression>(expr(*wh->body))};
    } else if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) {
      out = expr(*ct->e);  // become the inner expr; record the constraint as extra
      out.extras.push_back(tt::ExprExtra{core_type(*ct->t), e.loc});
    } else if (auto* fd = std::get_if<Pexp_field>(&e.desc)) {
      out.desc = tt::Texp_field{std::make_unique<tt::Expression>(expr(*fd->e)),
                                lid_str(fd->field.txt)};
    } else if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      out.desc = tt::Texp_setfield{std::make_unique<tt::Expression>(expr(*sf->obj)),
                                   lid_str(sf->field.txt),
                                   std::make_unique<tt::Expression>(expr(*sf->value))};
    } else if (auto* rec = std::get_if<Pexp_record>(&e.desc)) {
      tt::Texp_record tr;
      if (rec->base)
        tr.extended = std::make_unique<tt::Expression>(expr(**rec->base));
      const RecordInfo* info = nullptr;
      if (!rec->fields.empty()) {
        auto it = field_registry.find(lid_last(rec->fields[0].first.txt));
        if (it != field_registry.end()) info = &it->second;
      }
      if (info) {  // emit all fields in declaration order, <kept> for omitted
        std::unordered_map<std::string, std::pair<std::string, const ExprBox*>> prov;
        for (auto& [lid, ev] : rec->fields)
          prov[lid_last(lid.txt)] = {lid_str(lid.txt), &ev};
        tr.representation = info->repr;
        for (auto& fname : info->decl_fields) {
          tt::RecordField rf;
          auto p = prov.find(fname);
          if (p != prov.end()) {
            rf.name = p->second.first;
            rf.value = std::make_unique<tt::Expression>(expr(**p->second.second));
          } else {
            rf.kept = true;
          }
          tr.fields.push_back(std::move(rf));
        }
      } else {  // unknown record type: source order, all overridden
        for (auto& [lid, ev] : rec->fields) {
          tt::RecordField rf;
          rf.name = lid_str(lid.txt);
          rf.value = std::make_unique<tt::Expression>(expr(*ev));
          tr.fields.push_back(std::move(rf));
        }
      }
      out.desc = std::move(tr);
    } else if (auto* sti = std::get_if<Pexp_struct_item>(&e.desc)) {
      // let module/open/exception … in e: the item's bindings scope to the body.
      auto st = type_scope; auto md = module_scope; auto mt = modtype_scope;
      auto fr = field_registry; auto op = opens;
      push();
      auto item = std::make_unique<tt::StructureItem>(structure_item(*sti->item));
      auto body = std::make_unique<tt::Expression>(expr(*sti->body));
      pop();
      type_scope = std::move(st); module_scope = std::move(md);
      modtype_scope = std::move(mt); field_registry = std::move(fr);
      opens = std::move(op);
      out.desc = tt::Texp_struct_item{std::move(item), std::move(body)};
    } else {
      throw TypeError("expr#" + std::to_string(e.desc.index()));
    }
    return out;
  }

  // Transcribe a match/try case.  Match cases are computation patterns and so
  // wrap the value pattern in a Tpat_value layer (same loc); try cases are value
  // patterns and don't.
  tt::Case case_(const ast::Case& c, bool computation) {
    push();
    tt::Case out;
    out.lhs = computation ? to_computation(c.lhs) : pattern(c.lhs);
    if (c.guard)
      out.guard = std::make_unique<tt::Expression>(expr(**c.guard));
    out.rhs = std::make_unique<tt::Expression>(expr(*c.rhs));
    pop();
    return out;
  }

  tt::Texp_function function(const Pexp_function& f) {
    tt::Texp_function fn;
    push();
    for (auto& param : f.params) {
      auto* pv = std::get_if<Pparam_val>(&param.desc);
      if (!pv) { pop(); throw TypeError("unsupported function param"); }
      if (pv->default_) { pop(); throw TypeError("optional default param"); }
      tt::FunctionParam fp;
      fp.label = pv->label;
      fp.pat = std::make_unique<tt::Pattern>(pattern(pv->pat));
      fn.params.push_back(std::move(fp));
    }
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      fn.is_cases = false;
      fn.body = std::make_unique<tt::Expression>(expr(*fb->e));
    } else {
      auto& fc = std::get<Pfunction_cases>(f.body->v);
      fn.is_cases = true;
      fn.cases_loc = fc.loc;
      for (auto& c : fc.cases)
        fn.cases.push_back(case_(c, /*computation=*/false));  // value patterns
    }
    pop();
    return fn;
  }

  tt::ValueBinding value_binding(const ValueBinding& vb) {
    tt::ValueBinding out;
    out.expr = expr(*vb.expr);  // RHS typed before the pattern is bound (non-rec)
    out.pat = pattern(vb.pat);
    out.attrs = &vb.attrs;
    return out;
  }

  // For `let rec`, bind all pattern names before typing any RHS so the names are
  // in scope in their own and siblings' bodies.
  std::vector<tt::ValueBinding> value_bindings(RecFlag rf,
                                               const std::vector<ValueBinding>& vbs) {
    std::vector<tt::ValueBinding> out;
    if (rf == RecFlag::Recursive) {
      std::vector<tt::Pattern> pats;
      pats.reserve(vbs.size());
      for (auto& vb : vbs) pats.push_back(pattern(vb.pat));
      for (size_t i = 0; i < vbs.size(); ++i) {
        tt::ValueBinding b;
        b.pat = std::move(pats[i]);
        b.expr = expr(*vbs[i].expr);
        b.attrs = &vbs[i].attrs;
        out.push_back(std::move(b));
      }
    } else {
      for (auto& vb : vbs) out.push_back(value_binding(vb));
    }
    return out;
  }

  // A nested structure (module body) gets its own scopes; outer type/module/open
  // bindings are visible inside but inner ones don't leak out.
  std::vector<tt::StructureItem> nested_structure(const ast::Structure& s) {
    auto st = type_scope;
    auto md = module_scope;
    auto op = opens;
    push();
    std::vector<tt::StructureItem> out;
    for (auto& it : s) out.push_back(structure_item(it));
    pop();
    type_scope = std::move(st);
    module_scope = std::move(md);
    opens = std::move(op);
    return out;
  }

  // A signature gets its own scopes (its types/etc. don't leak out).
  std::vector<tt::SignatureItem> signature(const Signature& items) {
    auto st = type_scope; auto md = module_scope; auto mt = modtype_scope;
    auto fr = field_registry; auto op = opens;
    push();
    std::vector<tt::SignatureItem> out;
    for (auto& it : items) {
      tt::SignatureItem si;
      si.loc = it.loc;
      if (auto* v = std::get_if<Psig_value>(&it.desc)) {
        tt::ValueDesc vd;
        vd.id = fresh_anon(v->vd.name.txt);
        vd.type = core_type(*v->vd.type);
        vd.loc = v->vd.loc;
        vd.attrs = &v->vd.attrs;
        si.desc = tt::Tsig_value{std::move(vd)};
      } else if (auto* t = std::get_if<Psig_type>(&it.desc)) {
        tt::Tsig_type ts;
        ts.rf = t->rf;
        ts.decls = type_decls(t->decls);
        si.desc = std::move(ts);
      } else {
        throw TypeError("sigitem#" + std::to_string(it.desc.index()));
      }
      out.push_back(std::move(si));
    }
    pop();
    type_scope = std::move(st); module_scope = std::move(md);
    modtype_scope = std::move(mt); field_registry = std::move(fr);
    opens = std::move(op);
    return out;
  }

  tt::ModuleType module_type_t(const ModuleType& mt) {
    tt::ModuleType out;
    out.loc = mt.loc;
    if (auto* id = std::get_if<Pmty_ident>(&mt.desc)) {
      out.desc = tt::Tmty_ident{resolve_modtype(id->id.txt)};
    } else if (auto* sg = std::get_if<Pmty_signature>(&mt.desc)) {
      out.desc = tt::Tmty_signature{signature(sg->items)};
    } else {
      throw TypeError("module_type#" + std::to_string(mt.desc.index()));
    }
    return out;
  }

  tt::ModuleExpr module_expr(const ModuleExpr& me) {
    tt::ModuleExpr out;
    out.loc = me.loc;
    if (auto* mi = std::get_if<Pmod_ident>(&me.desc)) {
      out.desc = tt::Tmod_ident{resolve_module(mi->id.txt)};
    } else if (auto* ms = std::get_if<Pmod_structure>(&me.desc)) {
      out.desc = tt::Tmod_structure{nested_structure(ms->items)};
    } else if (auto* fn = std::get_if<Pmod_functor>(&me.desc)) {
      auto* named = std::get_if<Functor_named>(&fn->param);
      if (!named) throw TypeError("generative functor (unit param)");
      tt::Tmod_functor tf;
      auto saved = module_scope;
      tf.param = fresh_module(named->name.txt ? *named->name.txt : "_");
      tf.param_type = std::make_unique<tt::ModuleType>(module_type_t(*named->type));
      tf.body = std::make_unique<tt::ModuleExpr>(module_expr(*fn->body));
      module_scope = std::move(saved);
      out.desc = std::move(tf);
    } else if (auto* ap = std::get_if<Pmod_apply>(&me.desc)) {
      out.desc = tt::Tmod_apply{
          std::make_unique<tt::ModuleExpr>(module_expr(*ap->f)),
          std::make_unique<tt::ModuleExpr>(module_expr(*ap->arg))};
    } else if (auto* cn = std::get_if<Pmod_constraint>(&me.desc)) {
      out.desc = tt::Tmod_constraint{
          std::make_unique<tt::ModuleExpr>(module_expr(*cn->me)),
          std::make_unique<tt::ModuleType>(module_type_t(*cn->mt))};
    } else {
      throw TypeError("module_expr#" + std::to_string(me.desc.index()));
    }
    return out;
  }

  tt::StructureItem structure_item(const StructureItem& it) {
    tt::StructureItem si;
    si.loc = it.loc;
    if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
      tt::Tstr_value out;
      out.rf = sv->rf;
      out.bindings = value_bindings(sv->rf, sv->bindings);
      si.desc = std::move(out);
    } else if (auto* ev = std::get_if<Pstr_eval>(&it.desc)) {
      si.desc = tt::Tstr_eval{std::make_unique<tt::Expression>(expr(*ev->e))};
    } else if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      tt::Tstr_type out;
      out.rf = ty->rf;
      out.decls = type_decls(ty->decls);
      si.desc = std::move(out);
    } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
      if (!pr->prim.type || pr->prim.alias) throw TypeError("primitive alias");
      tt::Tstr_primitive tp;
      tp.id = fresh_anon(pr->prim.name.txt);
      tp.loc = pr->prim.loc;
      tp.type = core_type(*pr->prim.type);
      tp.prims = pr->prim.prims;
      si.desc = std::move(tp);
    } else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
      auto* mi = std::get_if<Pmod_ident>(&op->expr.desc);
      if (!mi) throw TypeError("open of non-ident module");
      tt::Path mpath = resolve_module(mi->id.txt);
      tt::Tstr_open to;
      to.override_ = op->ovr == OverrideFlag::Override;
      to.expr = std::make_unique<tt::ModuleExpr>();
      to.expr->loc = op->expr.loc;
      to.expr->desc = tt::Tmod_ident{mpath};  // copy; mpath reused below
      si.desc = std::move(to);
      // Bring the opened module's names into scope (stdlib modules, best effort).
      if (auto* l = std::get_if<Lident>(&mi->id.txt.v)) {
        OpenEntry oe;
        oe.path = std::move(mpath);
        load_open_names(l->name, oe);
        opens.push_back(std::move(oe));
      }
    } else if (auto* ex = std::get_if<Pstr_exception>(&it.desc)) {
      tt::Tstr_exception te;
      te.ctor = ext_ctor(ex->exn.ctor);
      te.attrs = &ex->exn.attrs;
      si.desc = std::move(te);
    } else if (auto* tx = std::get_if<Pstr_typext>(&it.desc)) {
      auto& e = tx->ext;
      tt::Tstr_typext out;
      out.path = resolve_type(e.path.txt);
      for (auto& p : e.params)
        out.params.push_back(std::make_unique<tt::CoreType>(core_type(*p)));
      for (auto& c : e.ctors) out.ctors.push_back(ext_ctor(c));
      out.private_ = e.priv == PrivateFlag::Private;
      out.attrs = &e.attrs;
      si.desc = std::move(out);
    } else if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
      tt::Tstr_modtype out;
      out.id = fresh_modtype(mt->name.txt);
      if (mt->type)
        out.type = std::make_unique<tt::ModuleType>(module_type_t(*mt->type));
      si.desc = std::move(out);
    } else if (auto* at = std::get_if<Pstr_attribute>(&it.desc)) {
      si.desc = tt::Tstr_attribute{at->name, &at->payload};
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      auto& b = mb->binding;
      tt::Tstr_module tm;
      tm.id = fresh_module(b.name.txt ? *b.name.txt : "_");
      tm.present = !std::holds_alternative<Pmod_ident>(b.expr.desc);  // alias=Absent
      tm.expr = std::make_unique<tt::ModuleExpr>(module_expr(b.expr));
      si.desc = std::move(tm);
    } else {
      throw TypeError("stritem#" + std::to_string(it.desc.index()));
    }
    return si;
  }
};

}  // namespace

typedtree::Structure type_structure(const ast::Structure& s) {
  Typer t;
  auto partiality = infer_match_partiality(s);  // inference side-table (Slice 3)
  t.partiality = &partiality;
  typedtree::Structure out;
  for (auto& it : s) out.push_back(t.structure_item(it));
  return out;
}

}  // namespace cppcaml
