#include "cppcaml/cmi.hpp"
#include "cppcaml/omarshal.hpp"
#include "cppcaml/blake2.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cppcaml::cmi {

namespace {

namespace m = marshal;

// Walks a decoded Marshal arena and reconstructs Types structures on demand.
class Decoder {
public:
  explicit Decoder(const m::Arena& arena) : arena_(arena) {}

  // type_expr is the transient_expr record { desc; level; scope; id };
  // field 0 is the type_desc.  Memoize by arena id so shared/cyclic graphs
  // map to shared/cyclic C++ nodes.
  TypePtr type(std::size_t id) {
    auto it = memo_.find(id);
    if (it != memo_.end()) return it->second;
    auto t = std::make_shared<TypeExpr>();
    memo_.emplace(id, t);
    decode_desc(arena_[id].fields.at(0), *t);
    return t;
  }

  // A signature is an OCaml list of signature_item; dispatch each item by its
  // constructor tag.
  Signature signature(std::size_t list_id) {
    Signature out;
    for (std::size_t cur = list_id; arena_[cur].kind == m::Value::Kind::Block &&
                                    !arena_[cur].fields.empty();) {
      const m::Value& cons = arena_[cur];  // tag 0, size 2: head :: tail
      const m::Value& item = arena_[cons.fields[0]];
      if (item.kind == m::Value::Kind::Block) {
        switch (item.tag) {
          case 0:  // Sig_value of Ident.t * value_description * visibility
            if (item.fields.size() == 3) {
              SigValue sv;
              sv.name = ident(item.fields[0]).name;
              // value_description: field 0 = val_type, field 1 = val_kind.
              const m::Value& vd = arena_[item.fields[1]];
              sv.type = type(vd.fields.at(0));
              // val_kind: Val_reg is the immediate constant 0; Val_prim (an
              // inlined %/C primitive) is a block and takes no runtime field.
              bool runtime = vd.fields.size() > 1 &&
                             arena_[vd.fields[1]].kind == m::Value::Kind::Int;
              out.order.push_back({Signature::OrderEnt::Value,
                                   (int)out.values.size(), runtime});
              if (runtime)
                out.fields.push_back(sv.name);
              else if (vd.fields.size() > 1) {
                // Val_prim of Primitive.description: tag-0 block whose field 0 is
                // the description record; its field 0 is prim_name ("%op"/C name).
                const m::Value& vk = arena_[vd.fields[1]];
                if (!vk.fields.empty()) {
                  const m::Value& desc = arena_[vk.fields[0]];
                  // description record: field 0 = prim_name, field 1 = prim_arity.
                  if (!desc.fields.empty()) sv.prim = arena_[desc.fields[0]].str;
                  if (desc.fields.size() > 1 &&
                      arena_[desc.fields[1]].kind == m::Value::Kind::Int)
                    sv.prim_arity = (int)arena_[desc.fields[1]].i;
                }
              }
              out.values.push_back(std::move(sv));
            }
            break;
          case 1:  // Sig_type of Ident.t * type_declaration * rec_status * vis
            if (item.fields.size() == 4) {
              TypeDecl td = type_declaration(item.fields[1]);
              td.name = ident(item.fields[0]).name;
              out.order.push_back({Signature::OrderEnt::Type,
                                   (int)out.types.size(), false});
              out.types.push_back(std::move(td));
            }
            break;
          case 2:  // Sig_typext of Ident * extension_constructor * status * vis
            if (item.fields.size() == 4) {
              ExtConstructor ec = ext_constructor(item.fields[1]);
              ec.name = ident(item.fields[0]).name;
              out.fields.push_back(ec.name);  // an exception/extension takes a field
              out.order.push_back({Signature::OrderEnt::Typext,
                                   (int)out.typexts.size(), true});
              out.typexts.push_back(std::move(ec));
            }
            break;
          case 3:  // Sig_module of Ident * presence * md * rec_status * vis
            if (item.fields.size() == 5) {
              ModuleDecl md;
              md.name = ident(item.fields[0]).name;
              // module_declaration.md_type is field 0 of the record.
              md.type = module_type(arena_[item.fields[2]].fields.at(0));
              // Only a present submodule (Mp_present = int 0) takes a runtime
              // field; an Mp_absent module alias is transparent (no field).
              const m::Value& pres = arena_[item.fields[1]];
              bool absent = (pres.kind == m::Value::Kind::Int && pres.i != 0);
              if (!absent) out.fields.push_back(md.name);
              out.order.push_back({Signature::OrderEnt::Module,
                                   (int)out.modules.size(), !absent});
              out.modules.push_back(std::move(md));
            }
            break;
          case 4:  // Sig_modtype of Ident * modtype_declaration * vis
            if (item.fields.size() == 3) {
              ModtypeDecl mtd;
              mtd.name = ident(item.fields[0]).name;
              // modtype_declaration.mtd_type is field 0: module_type option.
              const m::Value& opt = arena_[arena_[item.fields[1]].fields.at(0)];
              if (opt.kind == m::Value::Kind::Block && opt.tag == 0)
                mtd.type = module_type(opt.fields.at(0));
              out.order.push_back({Signature::OrderEnt::Modtype,
                                   (int)out.modtypes.size(), false});
              out.modtypes.push_back(std::move(mtd));
            }
            break;
          default:
            break;  // Sig_class / Sig_class_type: not decoded yet
        }
      }
      cur = cons.fields[1];  // tail
    }
    return out;
  }

  // module_type (typing/types.mli): Mty_ident / Mty_signature / Mty_functor /
  // Mty_alias.
  ModuleTypePtr module_type(std::size_t id) {
    const m::Value& v = arena_[id];
    auto mt = std::make_shared<ModuleType>();
    switch (v.tag) {
      case 0:  // Mty_ident of Path.t
        mt->kind = ModuleType::Ident;
        mt->path = path(v.fields.at(0));
        break;
      case 1:  // Mty_signature of signature
        mt->kind = ModuleType::Sig;
        mt->sig = std::make_shared<Signature>(signature(v.fields.at(0)));
        break;
      case 2:  // Mty_functor of functor_parameter * module_type
        mt->kind = ModuleType::Functor;
        functor_parameter(v.fields.at(0), *mt);
        mt->functor_body = module_type(v.fields.at(1));
        break;
      case 3:  // Mty_alias of Path.t
        mt->kind = ModuleType::Alias;
        mt->path = path(v.fields.at(0));
        break;
      default:
        break;
    }
    return mt;
  }

  void functor_parameter(std::size_t id, ModuleType& mt) {
    const m::Value& v = arena_[id];
    if (v.kind == m::Value::Kind::Int) {  // Unit
      mt.functor_unit = true;
      return;
    }
    // Named of Ident.t option * module_type
    const m::Value& name_opt = arena_[v.fields.at(0)];
    if (name_opt.kind == m::Value::Kind::Block && name_opt.tag == 0)
      mt.functor_param = ident(name_opt.fields.at(0)).name;
    mt.functor_param_type = module_type(v.fields.at(1));
  }

  ExtConstructor ext_constructor(std::size_t id) {
    const m::Value& v = arena_[id];  // { ext_type_path; ext_type_params;
                                     //   ext_args; ext_ret_type; ... }
    ExtConstructor ec;
    ec.type_path = path(v.fields.at(0));
    const m::Value& args = arena_[v.fields.at(2)];
    if (args.tag == 0) {  // Cstr_tuple
      ec.args = type_list(args.fields.at(0));
    } else {  // Cstr_record
      ec.is_inline_record = true;
      for (std::size_t cur = args.fields.at(0);
           arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
        const m::Value& cons = arena_[cur];
        ec.inline_record.push_back(label_decl(cons.fields[0]));
        cur = cons.fields[1];
      }
    }
    const m::Value& res = arena_[v.fields.at(3)];
    if (res.kind == m::Value::Kind::Block && res.tag == 0)
      ec.res = type(res.fields.at(0));
    return ec;
  }

  // type_declaration record: { type_params; type_arity; type_kind;
  // type_private; type_manifest; ... }.
  TypeDecl type_declaration(std::size_t id) {
    const m::Value& d = arena_[id];
    TypeDecl td;
    td.params = type_list(d.fields.at(0));
    if (arena_[d.fields.at(1)].kind == m::Value::Kind::Int)
      td.arity = static_cast<int>(arena_[d.fields[1]].i);
    type_kind(d.fields.at(2), td);
    // type_manifest : type_expr option (field 4).
    const m::Value& man = arena_[d.fields.at(4)];
    if (man.kind == m::Value::Kind::Block && man.tag == 0)
      td.manifest = type(man.fields.at(0));
    return td;
  }

  void type_kind(std::size_t id, TypeDecl& td) {
    const m::Value& k = arena_[id];
    if (k.kind == m::Value::Kind::Int) {  // Type_open (the only constant case)
      td.kind = TypeDecl::Open;
      return;
    }
    switch (k.tag) {
      case 0:  // Type_abstract of type_origin
        td.kind = TypeDecl::Abstract;
        break;
      case 1:  // Type_record of label_declaration list * record_representation
        td.kind = TypeDecl::Record;
        for (std::size_t cur = k.fields.at(0);
             arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
          const m::Value& cons = arena_[cur];
          td.labels.push_back(label_decl(cons.fields[0]));
          cur = cons.fields[1];
        }
        break;
      case 2:  // Type_variant of constructor_declaration list * variant_repr
        td.kind = TypeDecl::Variant;
        for (std::size_t cur = k.fields.at(0);
             arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
          const m::Value& cons = arena_[cur];
          td.ctors.push_back(ctor_decl(cons.fields[0]));
          cur = cons.fields[1];
        }
        break;
      case 3:  // Type_external of string
        td.kind = TypeDecl::External;
        td.external_name = arena_[k.fields.at(0)].str;
        break;
      default:
        td.kind = TypeDecl::Abstract;
        break;
    }
  }

  LabelDecl label_decl(std::size_t id) {
    const m::Value& v = arena_[id];  // { ld_id; ld_mutable; ld_atomic; ld_type; ...}
    LabelDecl ld;
    ld.name = ident(v.fields.at(0)).name;
    ld.mutable_ = arena_[v.fields.at(1)].kind == m::Value::Kind::Int &&
                  arena_[v.fields[1]].i != 0;  // mutable_flag: Mutable = 1
    ld.type = type(v.fields.at(3));
    return ld;
  }

  ConstructorDecl ctor_decl(std::size_t id) {
    const m::Value& v = arena_[id];  // { cd_id; cd_args; cd_res; ... }
    ConstructorDecl cd;
    cd.name = ident(v.fields.at(0)).name;
    const m::Value& args = arena_[v.fields.at(1)];  // constructor_arguments
    if (args.tag == 0) {  // Cstr_tuple of type_expr list
      cd.args = type_list(args.fields.at(0));
    } else {  // Cstr_record of label_declaration list
      cd.is_inline_record = true;
      for (std::size_t cur = args.fields.at(0);
           arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
        const m::Value& cons = arena_[cur];
        cd.inline_record.push_back(label_decl(cons.fields[0]));
        cur = cons.fields[1];
      }
    }
    const m::Value& res = arena_[v.fields.at(2)];  // cd_res : type_expr option
    if (res.kind == m::Value::Kind::Block && res.tag == 0)
      cd.res = type(res.fields.at(0));
    return cd;
  }

private:
  Ident ident(std::size_t id) {
    const m::Value& v = arena_[id];
    Ident out;
    out.kind = static_cast<Ident::Kind>(v.tag);
    if (!v.fields.empty()) out.name = arena_[v.fields[0]].str;
    if (v.fields.size() > 1 && arena_[v.fields[1]].kind == m::Value::Kind::Int)
      out.stamp = arena_[v.fields[1]].i;
    return out;
  }

  PathPtr path(std::size_t id) {
    const m::Value& v = arena_[id];
    auto p = std::make_shared<Path>();
    p->kind = static_cast<Path::Kind>(v.tag);
    switch (v.tag) {
      case Path::Pident:
        p->id = ident(v.fields.at(0));
        break;
      case Path::Pdot:
        p->a = path(v.fields.at(0));
        p->s = arena_[v.fields.at(1)].str;
        break;
      case Path::Papply:
        p->a = path(v.fields.at(0));
        p->b = path(v.fields.at(1));
        break;
      case Path::Pextra_ty:
        p->a = path(v.fields.at(0));
        break;
      default:
        break;
    }
    return p;
  }

  std::optional<std::string> opt_string(std::size_t id) {
    const m::Value& v = arena_[id];
    if (v.kind == m::Value::Kind::Int) return std::nullopt;  // None
    return arena_[v.fields.at(0)].str;                       // Some s
  }

  std::vector<TypePtr> type_list(std::size_t id) {
    std::vector<TypePtr> out;
    for (std::size_t cur = id; arena_[cur].kind == m::Value::Kind::Block &&
                               !arena_[cur].fields.empty();) {
      const m::Value& cons = arena_[cur];
      out.push_back(type(cons.fields[0]));
      cur = cons.fields[1];
    }
    return out;
  }

  void decode_desc(std::size_t id, TypeExpr& t) {
    const m::Value& d = arena_[id];
    if (d.kind == m::Value::Kind::Int) {
      // The only constant type_desc constructor is Tnil.
      t.kind = (d.i == 0) ? TypeExpr::Tnil : TypeExpr::Other;
      return;
    }
    switch (d.tag) {
      case 0:  // Tvar of string option
        t.kind = TypeExpr::Tvar;
        t.name = opt_string(d.fields.at(0));
        break;
      case 1:  // Tarrow of arg_label * type_expr * type_expr * commutable
        t.kind = TypeExpr::Tarrow;
        decode_arg_label(d.fields.at(0), t);
        t.dom = type(d.fields.at(1));
        t.cod = type(d.fields.at(2));
        break;
      case 2:  // Ttuple of (string option * type_expr) list
        t.kind = TypeExpr::Ttuple;
        for (std::size_t cur = d.fields.at(0);
             arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
          const m::Value& cons = arena_[cur];
          const m::Value& pair = arena_[cons.fields[0]];  // (label opt, te)
          t.elems.emplace_back(opt_string(pair.fields.at(0)), type(pair.fields.at(1)));
          cur = cons.fields[1];
        }
        break;
      case 3:  // Tconstr of Path.t * type_expr list * abbrev_memo ref
        t.kind = TypeExpr::Tconstr;
        t.path = path(d.fields.at(0));
        t.args = type_list(d.fields.at(1));
        break;
      case 11:  // Texpand of type_expr * Path.t * type_expr list
        t.kind = TypeExpr::Texpand;
        t.link = type(d.fields.at(0));
        t.path = path(d.fields.at(1));
        t.args = type_list(d.fields.at(2));
        break;
      case 12:  // Tlink of type_expr
        t.kind = TypeExpr::Tlink;
        t.link = type(d.fields.at(0));
        break;
      case 13:  // Tsubst of type_expr * type_expr option
        t.kind = TypeExpr::Tsubst;
        t.link = type(d.fields.at(0));
        break;
      case 7:  // Tunivar of string option
        t.kind = TypeExpr::Tunivar;
        t.name = opt_string(d.fields.at(0));
        break;
      case 8:  // Tpoly of type_expr * type_expr list
        t.kind = TypeExpr::Tpoly;
        t.link = type(d.fields.at(0));
        break;
      default:
        // Tobject/Tfield/Tvariant/Tpackage/Tfunctor: kind only for now.
        switch (d.tag) {
          case 4: t.kind = TypeExpr::Tobject; break;
          case 5: t.kind = TypeExpr::Tfield; break;
          case 6: {  // Tvariant of row_desc; row_desc.field0 = row_fields list.
            t.kind = TypeExpr::Tvariant;
            // row_desc = { row_fields:(label*row_field) list; row_more; .. }.
            if (d.fields.empty()) break;
            const m::Value& rd = arena_[d.fields[0]];
            if (!rd.fields.empty())
              for (std::size_t cur = rd.fields[0];
                   arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();
                   cur = arena_[cur].fields[1]) {       // cons cell: (head, tail)
                const m::Value& pair = arena_[arena_[cur].fields[0]];  // (label, row_field)
                if (!pair.fields.empty()) {
                  const m::Value& lbl = arena_[pair.fields[0]];
                  if (lbl.kind == m::Value::Kind::String) t.pv_tags.push_back(lbl.str);
                }
              }
            break;
          }
          case 9: t.kind = TypeExpr::Tpackage; break;
          case 10: t.kind = TypeExpr::Tfunctor; break;
          default: t.kind = TypeExpr::Other; break;
        }
        break;
    }
  }

  void decode_arg_label(std::size_t id, TypeExpr& t) {
    const m::Value& v = arena_[id];
    if (v.kind == m::Value::Kind::Int) {
      t.label_kind = 0;  // Nolabel
      return;
    }
    t.label_kind = v.tag == 0 ? 1 : 2;  // Labelled / Optional
    t.label = arena_[v.fields.at(0)].str;
  }

  const m::Arena& arena_;
  std::unordered_map<std::size_t, TypePtr> memo_;
};

}  // namespace

CmiFile CmiFile::load(const std::string& filepath) {
  // A .cmi is immutable for the lifetime of a compile, but several passes (the
  // inferencer, register_stdlib_ctors, pervasive resolution) each re-decode the
  // same file -- stdlib.cmi alone is decoded 3x.  Memoise by path: the Marshal
  // decode (the costly part, ~2 ms for stdlib.cmi) then happens once.
  static std::unordered_map<std::string, CmiFile> cache;
  if (auto it = cache.find(filepath); it != cache.end()) return it->second;
  std::ifstream in(filepath, std::ios::binary);
  if (!in) throw m::Error("cannot open " + filepath);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());

  // Skip the cmi magic string and decode the header value (name, signature).
  std::size_t off = 0;
  for (; off + 4 <= bytes.size(); ++off)
    if (bytes[off] == 0x84 && bytes[off + 1] == 0x95 && bytes[off + 2] == 0xA6 &&
        (bytes[off + 3] == 0xBE || bytes[off + 3] == 0xBF || bytes[off + 3] == 0xBD))
      break;
  if (off + 4 > bytes.size()) throw m::Error("no Marshal magic in " + filepath);

  m::Arena arena;
  std::size_t header = m::read_value(bytes.data(), bytes.size(), off, arena);
  const m::Value& tuple = arena[header];  // (modname, signature)

  Decoder dec(arena);
  CmiFile cmi;
  cmi.module_name_ = arena[tuple.fields.at(0)].str;
  cmi.sig_ = dec.signature(tuple.fields.at(1));
  cache.emplace(filepath, cmi);
  return cmi;
}

const SigValue* CmiFile::find_value(const std::string& name) const {
  for (const auto& v : sig_.values)
    if (v.name == name) return &v;
  return nullptr;
}

const TypeDecl* CmiFile::find_type(const std::string& name) const {
  for (const auto& t : sig_.types)
    if (t.name == name) return &t;
  return nullptr;
}

const ModuleDecl* CmiFile::find_module(const std::string& name) const {
  for (const auto& m : sig_.modules)
    if (m.name == name) return &m;
  return nullptr;
}

namespace {
// The namespace a runtime field belongs to, recovered from the grouped signature:
// 1 = submodule, 2 = exception (a typext of the predef `exn`), 0 = value.  -1 if
// the name is AMBIGUOUS across namespaces (a `val x` and a `module x` both take a
// field) -- the bare `fields` list can't disambiguate two same-named fields, so
// the caller bails to keep today's behavior (a rare case; the substrate gains a
// per-field namespace later).  3 = none (shouldn't appear in `fields`).
int field_ns(const Signature& s, const std::string& name) {
  int hits = 0, ns = 3;
  for (auto& m : s.modules) if (m.name == name) { ns = 1; ++hits; break; }
  for (auto& x : s.typexts) if (x.name == name) { if (ns != 1) ns = 2; ++hits; break; }
  for (auto& v : s.values) if (v.name == name && v.prim.empty()) { if (ns == 3) ns = 0; ++hits; break; }
  return hits > 1 ? -1 : ns;
}
// The inline signature of a submodule field, if its module type is a plain Sig.
const Signature* submodule_signature(const Signature& s, const std::string& name) {
  for (auto& m : s.modules)
    if (m.name == name && m.type && m.type->kind == ModuleType::Sig && m.type->sig)
      return m.type->sig.get();
  return nullptr;
}
}  // namespace

ModCoercion compute_coercion(const Signature& src, const Signature& tgt) {
  ModCoercion c;
  c.fields.reserve(tgt.fields.size());
  for (const std::string& tn : tgt.fields) {
    int tns = field_ns(tgt, tn);
    if (tns < 0 || tns == 3) { c.ok = false; c.error = tn; return c; }  // ambiguous/none
    int sidx = -1;
    for (int si = 0; si < (int)src.fields.size(); ++si)
      if (src.fields[si] == tn && field_ns(src, src.fields[si]) == tns) { sidx = si; break; }
    if (sidx < 0) { c.ok = false; c.error = tn; return c; }  // member absent in source
    ModCoercion::Field f;
    f.src_field = sidx;
    if (tns == 1) {  // a submodule: coerce it recursively when both sides have a Sig
      const Signature* ssub = submodule_signature(src, tn);
      const Signature* tsub = submodule_signature(tgt, tn);
      if (ssub && tsub) {
        ModCoercion sub = compute_coercion(*ssub, *tsub);
        if (!sub.ok) { c.ok = false; c.error = tn + "." + sub.error; return c; }
        if (!sub.identity) f.sub = std::make_shared<ModCoercion>(std::move(sub));
      }
    }
    c.fields.push_back(std::move(f));
  }
  c.identity = c.fields.size() == src.fields.size();
  for (size_t i = 0; i < c.fields.size() && c.identity; ++i)
    if (c.fields[i].src_field != (int)i || c.fields[i].sub) c.identity = false;
  return c;
}

namespace {

const TypeExpr* follow(const TypeExpr* t) {
  while (t && (t->kind == TypeExpr::Tlink || t->kind == TypeExpr::Tsubst) && t->link)
    t = t->link.get();
  return t;
}

std::string path_last_name(const Path& p) {
  switch (p.kind) {
    case Path::Pident: return p.id.name;
    case Path::Pdot: return p.s;
    case Path::Papply:
      return (p.a ? path_last_name(*p.a) : "?") + "(" +
             (p.b ? path_last_name(*p.b) : "?") + ")";
    case Path::Pextra_ty: return p.a ? path_last_name(*p.a) : "?";
  }
  return "?";
}

std::string path_full(const Path& p) {
  switch (p.kind) {
    case Path::Pident: return p.id.name;
    case Path::Pdot: return (p.a ? path_full(*p.a) : "?") + "." + p.s;
    case Path::Papply:
      return (p.a ? path_full(*p.a) : "?") + "(" +
             (p.b ? path_full(*p.b) : "?") + ")";
    case Path::Pextra_ty: return p.a ? path_full(*p.a) : "?";
  }
  return "?";
}

void print_rec(const TypePtr& tp, std::string& out, bool arrow_paren);

void print_args(const std::vector<TypePtr>& args, const Path& p, std::string& out) {
  if (args.empty()) {
    out += path_last_name(p);
    return;
  }
  if (args.size() == 1) {
    print_rec(args[0], out, true);
    out += " ";
    out += path_last_name(p);
    return;
  }
  out += "(";
  for (std::size_t k = 0; k < args.size(); ++k) {
    if (k) out += ", ";
    print_rec(args[k], out, false);
  }
  out += ") ";
  out += path_last_name(p);
}

void print_rec(const TypePtr& tp, std::string& out, bool arrow_paren) {
  const TypeExpr* t = follow(tp.get());
  if (!t) { out += "_"; return; }
  switch (t->kind) {
    case TypeExpr::Tvar:
      out += t->name ? "'" + *t->name : "'_";
      break;
    case TypeExpr::Tunivar:
      out += t->name ? "'" + *t->name : "'_";
      break;
    case TypeExpr::Tarrow: {
      if (arrow_paren) out += "(";
      if (t->label_kind == 1) out += t->label + ":";
      else if (t->label_kind == 2) out += "?" + t->label + ":";
      // re-wrap the shared_ptr for recursion via the decoded children
      print_rec(t->dom, out, true);
      out += " -> ";
      print_rec(t->cod, out, false);
      if (arrow_paren) out += ")";
      break;
    }
    case TypeExpr::Ttuple:
      if (arrow_paren) out += "(";
      for (std::size_t k = 0; k < t->elems.size(); ++k) {
        if (k) out += " * ";
        if (t->elems[k].first) out += *t->elems[k].first + ":";
        print_rec(t->elems[k].second, out, true);
      }
      if (arrow_paren) out += ")";
      break;
    case TypeExpr::Tconstr:
    case TypeExpr::Texpand:
      if (t->path) print_args(t->args, *t->path, out);
      else out += "?";
      break;
    case TypeExpr::Tpoly:
      print_rec(t->link, out, arrow_paren);
      break;
    case TypeExpr::Tnil: out += "<nil>"; break;
    case TypeExpr::Tobject: out += "<obj>"; break;
    case TypeExpr::Tvariant: out += "[variant]"; break;
    default: out += "<?>"; break;
  }
}

}  // namespace

std::string print_type(const TypePtr& t) {
  std::string out;
  print_rec(t, out, false);
  return out;
}

std::string print_module_type(const ModuleType& mt) {
  switch (mt.kind) {
    case ModuleType::Ident:
      return mt.path ? path_full(*mt.path) : "?";
    case ModuleType::Alias:
      return "(= " + (mt.path ? path_full(*mt.path) : "?") + ")";
    case ModuleType::Sig: {
      std::size_t nv = mt.sig ? mt.sig->values.size() : 0;
      std::size_t nt = mt.sig ? mt.sig->types.size() : 0;
      std::size_t nm = mt.sig ? mt.sig->modules.size() : 0;
      return "sig <" + std::to_string(nv) + " val, " + std::to_string(nt) +
             " type, " + std::to_string(nm) + " mod> end";
    }
    case ModuleType::Functor: {
      std::string p = mt.functor_unit
                          ? "()"
                          : (mt.functor_param ? *mt.functor_param : "_") + " : " +
                                (mt.functor_param_type
                                     ? print_module_type(*mt.functor_param_type)
                                     : "?");
      return "functor (" + p + ") -> " +
             (mt.functor_body ? print_module_type(*mt.functor_body) : "?");
    }
  }
  return "?";
}

std::string print_type_decl(const TypeDecl& d) {
  std::string out = "type ";
  if (d.params.size() == 1) {
    print_rec(d.params[0], out, true);
    out += " ";
  } else if (d.params.size() > 1) {
    out += "(";
    for (std::size_t k = 0; k < d.params.size(); ++k) {
      if (k) out += ", ";
      print_rec(d.params[k], out, false);
    }
    out += ") ";
  }
  out += d.name;
  if (d.manifest) {
    out += " = ";
    out += print_type(d.manifest);
  }
  switch (d.kind) {
    case TypeDecl::Record: {
      out += " = { ";
      for (std::size_t k = 0; k < d.labels.size(); ++k) {
        if (k) out += "; ";
        if (d.labels[k].mutable_) out += "mutable ";
        out += d.labels[k].name + " : " + print_type(d.labels[k].type);
      }
      out += " }";
      break;
    }
    case TypeDecl::Variant: {
      out += " = ";
      for (std::size_t k = 0; k < d.ctors.size(); ++k) {
        if (k) out += " | ";
        const ConstructorDecl& c = d.ctors[k];
        out += c.name;
        if (c.is_inline_record) {
          out += " of { ";
          for (std::size_t j = 0; j < c.inline_record.size(); ++j) {
            if (j) out += "; ";
            out += c.inline_record[j].name + " : " +
                   print_type(c.inline_record[j].type);
          }
          out += " }";
        } else if (!c.args.empty()) {
          out += " of ";
          for (std::size_t j = 0; j < c.args.size(); ++j) {
            if (j) out += " * ";
            out += print_type(c.args[j]);
          }
        }
        if (c.res) out += " : " + print_type(c.res);  // GADT
      }
      break;
    }
    case TypeDecl::Open:
      out += " = ..";
      break;
    case TypeDecl::External:
      out += " = external \"" + d.external_name + "\"";
      break;
    case TypeDecl::Abstract:
      break;
  }
  return out;
}

// ---- .cmi WRITER ---------------------------------------------------------
namespace cmiw {

namespace o = omarshal;
TyPtr ty_predef(const std::string& n) { auto t = std::make_shared<Ty>(); t->k = Ty::Constr; t->name = n; return t; }
TyPtr ty_constr(const std::string& n, std::vector<TyPtr> as) { auto t = std::make_shared<Ty>(); t->k = Ty::Constr; t->name = n; t->args = std::move(as); return t; }
TyPtr ty_arrow(const TyPtr& d, const TyPtr& c) { auto t = std::make_shared<Ty>(); t->k = Ty::Arrow; t->args = {d, c}; return t; }
TyPtr ty_arrow_lbl(const TyPtr& d, const TyPtr& c, int lk, const std::string& lbl) {
  auto t = std::make_shared<Ty>(); t->k = Ty::Arrow; t->args = {d, c};
  t->label_kind = lk; t->label = lbl; return t; }
TyPtr ty_tuple(std::vector<TyPtr> es) { auto t = std::make_shared<Ty>(); t->k = Ty::Tuple; t->args = std::move(es); return t; }
TyPtr ty_variant(std::vector<std::string> tags) { auto t = std::make_shared<Ty>(); t->k = Ty::Variant; t->pv_tags = std::move(tags); return t; }
TyPtr ty_variant_row(std::vector<std::string> tags, std::vector<TyPtr> args,
                     int row_kind, std::vector<std::string> present) {
  auto t = std::make_shared<Ty>(); t->k = Ty::Variant;
  t->pv_tags = std::move(tags); t->args = std::move(args);
  t->row_kind = row_kind; t->pv_present = std::move(present); return t; }
TyPtr ty_object(std::vector<std::string> names, std::vector<TyPtr> tys) {
  auto t = std::make_shared<Ty>(); t->k = Ty::Object;
  t->pv_tags = std::move(names); t->args = std::move(tys); return t; }
TyPtr ty_package(std::string mty, std::vector<std::string> cnames, std::vector<TyPtr> ctys) {
  auto t = std::make_shared<Ty>(); t->k = Ty::Package; t->name = std::move(mty);
  t->pv_tags = std::move(cnames); t->args = std::move(ctys); return t; }
TyPtr ty_var(int id) { auto t = std::make_shared<Ty>(); t->k = Ty::Var; t->var = id; return t; }

namespace {
constexpr long long GENERIC_LEVEL = 100000000;
// Predefined type identifier stamps (typing/predef.ml create order, predefstamp
// counter): a predef Tconstr must carry the right stamp so a reader's Path.same
// resolves it to the real predefined type (e.g. `int` must unify with `+`).
int predef_stamp(const std::string& n) {
  static const std::unordered_map<std::string, int> s = {
      {"int", 1}, {"char", 2}, {"bytes", 3}, {"float", 4}, {"bool", 5},
      {"unit", 6}, {"exn", 7}, {"eff", 8}, {"continuation", 9}, {"array", 10},
      {"list", 11}, {"option", 12}, {"nativeint", 13}, {"int32", 14},
      {"int64", 15}, {"lazy_t", 16}, {"string", 17}, {"extension_constructor", 18},
      {"floatarray", 19}};
  auto it = s.find(n);
  return it == s.end() ? 0 : it->second;
}

// --- module resolution (for qualified `M.t` Tconstr paths + import CRCs) -----
std::string g_stdlib_dir = "stdlib";
std::vector<std::string> g_module_dirs;

// A source module head ("Buffer", "List", a local "A") -> its compilation-unit
// global ("Stdlib__Buffer", "A").  Mirrors lambda's global_of.
std::string global_of(const std::string& mod) {
  if (mod == "Stdlib" || mod.rfind("Stdlib__", 0) == 0) return mod;
  if (mod.rfind("Camlinternal", 0) == 0) return mod;
  if (std::filesystem::exists(g_stdlib_dir + "/stdlib__" + mod + ".cmi"))
    return "Stdlib__" + mod;
  return mod;  // a separately-compiled local module
}

// Bare type names declared at stdlib.cmi's own top level (`ref`,
// `in_channel`, `format4`, ...): in source they resolve through the implicit
// `open Stdlib`, and ocamlc stores them as Pdot(Pident(Global Stdlib), name).
// Loaded lazily from stdlib.cmi itself so the set tracks the tree.
bool stdlib_toplevel_type(const std::string& n) {
  static std::set<std::string>* names = nullptr;
  if (!names) {
    names = new std::set<std::string>();
    try {
      for (const auto& d : CmiFile::load(g_stdlib_dir + "/stdlib.cmi").types())
        names->insert(d.name);
    } catch (const std::exception&) {}  // -nostdlib: set stays empty
  }
  return names->count(n) > 0;
}

// A compilation-unit global -> its .cmi path.  Mirrors lambda's resolve_cmi but
// keyed by the already-resolved global name.
std::string resolve_cmi_global(const std::string& g) {
  if (g == "Stdlib") return g_stdlib_dir + "/stdlib.cmi";
  if (g.rfind("Stdlib__", 0) == 0)
    return g_stdlib_dir + "/stdlib__" + g.substr(8) + ".cmi";
  if (g.rfind("Camlinternal", 0) == 0)
    return g_stdlib_dir + "/" + (char)std::tolower((unsigned char)g[0]) + g.substr(1) + ".cmi";
  std::string low = (char)std::tolower((unsigned char)g[0]) + g.substr(1);
  for (const std::string& d : g_module_dirs) {
    if (std::filesystem::exists(d + "/" + low + ".cmi")) return d + "/" + low + ".cmi";
    if (std::filesystem::exists(d + "/" + g + ".cmi")) return d + "/" + g + ".cmi";
  }
  return g_stdlib_dir + "/stdlib__" + g + ".cmi";
}

// Read a .cmi's own interface CRC (the first crcs entry, whose name is the
// module itself).  Empty on any failure -- the caller then omits the import.
std::string read_cmi_self_crc(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  std::size_t off = 0;
  for (; off + 4 <= bytes.size(); ++off)
    if (bytes[off] == 0x84 && bytes[off + 1] == 0x95 && bytes[off + 2] == 0xA6 &&
        (bytes[off + 3] == 0xBE || bytes[off + 3] == 0xBF || bytes[off + 3] == 0xBD))
      break;
  if (off + 4 > bytes.size()) return "";
  try {
    m::Arena arena;
    m::read_value(bytes.data(), bytes.size(), off, arena);          // header
    std::size_t crcs = m::read_value(bytes.data(), bytes.size(), off, arena);  // crc list
    const m::Value& cell = arena[crcs];                             // first cons cell
    if (cell.kind != m::Value::Kind::Block || cell.fields.size() < 1) return "";
    const m::Value& entry = arena[cell.fields[0]];                  // (name, crc option)
    if (entry.kind != m::Value::Kind::Block || entry.fields.size() < 2) return "";
    const m::Value& crcopt = arena[entry.fields[1]];               // None=Int 0 / Some=Block{str}
    if (crcopt.kind != m::Value::Kind::Block || crcopt.fields.empty()) return "";
    return arena[crcopt.fields[0]].str;
  } catch (const std::exception&) {
    return "";
  }
}

// Build the omarshal value graph for one exported value's type.  `id` is a
// per-item counter for the cosmetic type_expr id field; `vars` shares Tvar
// nodes of equal identity (so `'a -> 'a` is one node, used twice).
struct TyEmit {
  long long id = -2;
  std::unordered_map<int, o::ValPtr> vars;
  // global module names cited by the signature -> whether they need a real
  // interface CRC (true for a qualified type like Buffer.t; false for a module
  // alias `module M = Unit`, which OCaml imports with CRC=None to avoid a
  // circular dependency, e.g. stdlib.cmi <-> stdlib__List.cmi).
  std::map<std::string, bool>* referenced = nullptr;
  const std::unordered_map<std::string, int>* local_types = nullptr;  // same-sig type -> stamp
  const std::unordered_map<std::string, int>* local_modtypes = nullptr;  // same-sig modtype -> stamp
  o::ValPtr texpr(o::ValPtr desc) {  // type_expr = {desc; level; scope; id}
    return o::vblock(0, {desc, o::vint(GENERIC_LEVEL), o::vint(0), o::vint(id--)});
  }
  // The Path.t for a Tpackage's modtype name: dotted through the head unit's
  // global (importing it); bare via the visible modtype map's Local stamp.
  o::ValPtr mty_path(const std::string& ref) {
    if (auto dot = ref.find('.'); dot != std::string::npos) {
      std::string head = global_of(ref.substr(0, dot));
      if (referenced) (*referenced)[head] = true;
      o::ValPtr path;
      if (head.rfind("Stdlib__", 0) == 0) {
        // a pervasive head goes THROUGH the Stdlib alias (Set.OrderedType =
        // Pdot(Pdot(Pident(Global Stdlib), "Set"), ..)), like type_path
        if (referenced) (*referenced)["Stdlib"] = true;
        path = o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})});
        path = o::vblock(1, {path, o::vstr(head.substr(8))});
      } else {
        path = o::vblock(0, {o::vblock(2, {o::vstr(head)})});  // Pident(Global)
      }
      for (std::size_t pos = dot; pos != std::string::npos;) {
        std::size_t nd = ref.find('.', pos + 1);
        path = o::vblock(1, {path, o::vstr(ref.substr(pos + 1,
            nd == std::string::npos ? std::string::npos : nd - pos - 1))});  // Pdot
        pos = nd;
      }
      return path;
    }
    if (local_modtypes)
      if (auto it = local_modtypes->find(ref); it != local_modtypes->end())
        return o::vblock(0, {o::vblock(0, {o::vstr(ref), o::vint(it->second)})});  // Pident(Local)
    return nullptr;
  }
  // The Path.t for a type-ctor name, via the resolution ladder: dotted name ->
  // Pdot chain off the head's compilation-unit global (a Stdlib__ head goes
  // THROUGH the Stdlib alias module -- `Buffer.t` is stored
  // Pdot(Pdot(Pident(Global Stdlib), "Buffer"), "t"), never the mangled unit,
  // and cites Stdlib's CRC too, like ocamlc); predef; sig-local declaration;
  // bare Stdlib-toplevel type (`ref`, resolved through the implicit
  // `open Stdlib`).  Null when the name can't be placed.
  o::ValPtr type_path(const std::string& name) {
    if (auto dot = name.find('.'); dot != std::string::npos) {
      std::vector<std::string> comps;
      for (std::size_t i = 0, j; i <= name.size(); i = j + 1) {
        j = name.find('.', i);
        if (j == std::string::npos) j = name.size();
        comps.push_back(name.substr(i, j - i));
      }
      std::string g = global_of(comps[0]);
      if (referenced) (*referenced)[g] = true;  // a real type ref needs the CRC
      o::ValPtr path;
      if (g.rfind("Stdlib__", 0) == 0) {
        if (referenced) (*referenced)["Stdlib"] = true;
        path = o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})});  // Pident(Global Stdlib)
        path = o::vblock(1, {path, o::vstr(g.substr(8))});         // Pdot(_, alias member)
      } else {
        path = o::vblock(0, {o::vblock(2, {o::vstr(g)})});  // Pident(Global head)
      }
      for (std::size_t i = 1; i < comps.size(); ++i)
        path = o::vblock(1, {path, o::vstr(comps[i])});   // Pdot(path, comp)
      return path;
    }
    if (int st = predef_stamp(name))
      return o::vblock(0, {o::vblock(3, {o::vstr(name), o::vint(st)})});  // Pident(Predef)
    if (local_types && local_types->count(name)) {
      int st = local_types->at(name);
      return o::vblock(0, {o::vblock(0, {o::vstr(name), o::vint(st)})});  // Pident(Local)
    }
    if (stdlib_toplevel_type(name)) {
      if (referenced) (*referenced)["Stdlib"] = true;
      return o::vblock(1, {o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})}),
                           o::vstr(name)});  // Pdot(Pident(Global Stdlib), name)
    }
    return nullptr;
  }
  // Object/Variant nodes shared within one scheme must marshal as ONE node
  // (omarshal CODE_SHARED back-reference) so the reader sees the sharing and
  // Printtyp names the row `as 'a` -- two structural copies print unnamed.
  std::unordered_map<const Ty*, o::ValPtr> shared_nodes;
  o::ValPtr emit(const TyPtr& t) {
    if (t->k == Ty::Object || t->k == Ty::Variant)
      if (auto it = shared_nodes.find(t.get()); it != shared_nodes.end())
        return it->second;
    o::ValPtr res = emit_fresh(t);
    if (t->k == Ty::Object || t->k == Ty::Variant) shared_nodes[t.get()] = res;
    return res;
  }
  o::ValPtr emit_fresh(const TyPtr& t) {
    switch (t->k) {
      case Ty::Var: {
        if (auto it = vars.find(t->var); it != vars.end()) return it->second;
        o::ValPtr nm = t->var_name.empty()
                           ? o::vint(0)                                // None
                           : o::vblock(0, {o::vstr(t->var_name)});     // Some name
        o::ValPtr te = texpr(o::vblock(0, {nm}));  // Tvar
        vars[t->var] = te;
        return te;
      }
      case Ty::Constr: {
        // A qualified name (`Buffer.t`, `A.Inner.t`) emits a Tconstr whose path
        // is Pdot(...Pdot(Pident(Global head), mid)..., typename), with `head`
        // resolved to its compilation-unit global; the cited unit is recorded so
        // write_cmi can list it (with its CRC) among the imports.  A predefined
        // name emits Pident(Predef) with the predef.ml stamp.  Anything else (a
        // bare user/local type we can't yet place) degrades to an opaque Tvar --
        // valid, just over-general.
        o::ValPtr path = type_path(t->name);
        if (!path) return texpr(o::vblock(0, {o::vint(0)}));  // unknown -> Tvar None
        std::vector<o::ValPtr> as;
        for (auto& a : t->args) as.push_back(emit(a));
        auto abbrev = o::vblock(0, {o::vint(0)});  // ref Mnil
        return texpr(o::vblock(3, {path, as.empty() ? o::vint(0) : o::vlist(as), abbrev}));  // Tconstr
      }
      case Ty::Arrow: {
        // In this trunk a Tarrow's DOMAIN is wrapped in Tpoly(ty, []) (to allow
        // first-class-poly arguments); the codomain stays bare.  OCaml asserts
        // (btype.tpoly_get_mono) if the argument isn't a Tpoly.
        o::ValPtr inner = emit(t->args[0]);
        if (t->label_kind == 2 &&
            !(t->args[0]->k == Ty::Constr && t->args[0]->name == "option")) {
          // An OPTIONAL argument's stored domain is `d option` (the printer
          // strips it back to `?x:d`; a bare domain prints `?x:<hidden>`).
          auto opath = o::vblock(0, {o::vblock(3, {o::vstr("option"), o::vint(12)})});
          inner = texpr(o::vblock(3, {opath, o::vlist({inner}),
                                      o::vblock(0, {o::vint(0)})}));  // Tconstr option
        }
        o::ValPtr dom = texpr(o::vblock(8, {inner, o::vint(0) /*[]*/}));  // Tpoly
        o::ValPtr c = emit(t->args[1]);
        // arg_label = Nolabel (int 0) | Labelled of string (block tag 0)
        //           | Optional of string (block tag 1)
        o::ValPtr lbl = t->label_kind == 1 ? o::vblock(0, {o::vstr(t->label)})
                      : t->label_kind == 2 ? o::vblock(1, {o::vstr(t->label)})
                      : o::vint(0) /*Nolabel*/;
        return texpr(o::vblock(1, {lbl, dom, c, o::vint(0) /*Cok*/}));  // Tarrow
      }
      case Ty::Tuple: {
        std::vector<o::ValPtr> elems;
        for (auto& e : t->args) elems.push_back(o::vblock(0, {o::vint(0) /*None*/, emit(e)}));  // (label,ty)
        return texpr(o::vblock(2, {o::vlist(elems)}));  // Ttuple of (so * te) list
      }
      case Ty::Variant: {
        // A polymorphic-variant row.  ocamlc stores row_fields in
        // REVERSE-alphabetical order (the printer reads them back reversed, so
        // the printed row is alphabetical); an exact row's row_more is Tnil, an
        // open/upper one's a Tvar.  Fields are RFpresent(arg option), except an
        // upper `[<` row's non-present tags which are RFeither{no_arg; arg_type;
        // matched=false; ext=ref RFnone}.
        // row_desc = { row_fields; row_more; row_closed; row_fixed; row_name }.
        std::vector<std::size_t> ord(t->pv_tags.size());
        for (std::size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) {
          return t->pv_tags[a] > t->pv_tags[b];
        });
        std::unordered_set<std::string> present(t->pv_present.begin(),
                                                t->pv_present.end());
        std::vector<o::ValPtr> fields;
        for (std::size_t i : ord) {
          TyPtr arg = i < t->args.size() ? t->args[i] : nullptr;
          o::ValPtr rf;
          if (t->row_kind == 1 && !present.count(t->pv_tags[i])) {
            rf = o::vblock(1, {o::vint(arg ? 0 : 1) /*no_arg*/,
                               arg ? o::vlist({emit(arg)}) : o::vint(0) /*arg_type*/,
                               o::vint(0) /*matched=false*/,
                               o::vblock(0, {o::vint(1)}) /*ext=ref RFnone*/});  // RFeither
          } else {
            rf = o::vblock(0, {arg ? o::vblock(0, {emit(arg)}) : o::vint(0)});  // RFpresent
          }
          fields.push_back(o::vblock(0, {o::vstr(t->pv_tags[i]), rf}));  // (label, row_field)
        }
        o::ValPtr more = t->row_kind == 2
                             ? texpr(o::vint(0))                    // Tnil (exact)
                             : texpr(o::vblock(0, {o::vint(0)}));   // Tvar None
        o::ValPtr rd = o::vblock(0, {fields.empty() ? o::vint(0) : o::vlist(fields),
                                     more, o::vint(t->row_kind != 0 ? 1 : 0) /*row_closed*/,
                                     o::vint(0) /*row_fixed=None*/, o::vint(0) /*row_name=None*/});
        return texpr(o::vblock(6, {rd}));  // Tvariant of row_desc
      }
      case Ty::Object: {
        // A closed structural object type `< m1 : t1; m2 : t2 >`:
        // Tobject(Tfield(m1, FKpublic, Tpoly(t1,[]), ... Tnil), ref None).
        // Each method type is Tpoly-wrapped (ocamlc stores even monomorphic
        // methods as Tpoly(ty, [])); the row terminates in Tnil (closed).
        // The printer sorts fields by name, so emission order is source order.
        o::ValPtr row = texpr(o::vint(0));  // Tnil (a full type_expr node)
        for (std::size_t i = t->pv_tags.size(); i-- > 0;) {
          o::ValPtr mty = texpr(o::vblock(8, {emit(t->args[i]), o::vint(0)}));  // Tpoly(ty,[])
          row = texpr(o::vblock(5, {o::vstr(t->pv_tags[i]), o::vint(1) /*FKpublic*/,
                                    mty, row}));  // Tfield
        }
        return texpr(o::vblock(4, {row, o::vblock(0, {o::vint(0)})}));  // Tobject(row, ref None)
      }
      case Ty::Package: {
        // Tpackage { pack_path; pack_constraints: (string list * type_expr) list }.
        o::ValPtr path = mty_path(t->name);
        if (!path) return texpr(o::vblock(0, {o::vint(0)}));  // unplaceable -> Tvar None
        std::vector<o::ValPtr> cs;
        for (std::size_t i = 0; i < t->pv_tags.size() && i < t->args.size(); ++i) {
          // the constraint's type name, split on dots into a string list
          std::vector<o::ValPtr> comps;
          const std::string& n = t->pv_tags[i];
          for (std::size_t s = 0, e; s <= n.size(); s = e + 1) {
            e = n.find('.', s);
            if (e == std::string::npos) e = n.size();
            comps.push_back(o::vstr(n.substr(s, e - s)));
            if (e == n.size()) break;
          }
          cs.push_back(o::vblock(0, {o::vlist(comps), emit(t->args[i])}));
        }
        auto package = o::vblock(0, {path, cs.empty() ? o::vint(0) : o::vlist(cs)});
        return texpr(o::vblock(9, {package}));  // Tpackage
      }
    }
    return o::vint(0);
  }
};

o::ValPtr dummy_pos() {  // Lexing.dummy_pos = {pos_fname=""; pos_lnum=0; pos_bol=0; pos_cnum=-1}
  return o::vblock(0, {o::vstr(""), o::vint(0), o::vint(0), o::vint(-1)});
}
o::ValPtr loc_none() {  // Location.none = {loc_start; loc_end; loc_ghost=true}
  auto p = dummy_pos();
  return o::vblock(0, {p, p, o::vint(1)});
}
}  // namespace

// Marshal a list of signature items (recursive: a submodule's items nest under
// Mty_signature).  `stamp` is a counter shared across the whole cmi so every
// local ident is unique (a value's type referencing a same-module `type t`
// must cite that decl's exact stamp).
static std::vector<o::ValPtr> emit_sig_items(const std::vector<SigItem>& items,
                                             std::map<std::string, bool>& referenced,
                                             int& stamp,
                                             const std::unordered_map<std::string, int>* outer_types = nullptr,
                                             const std::unordered_map<std::string, int>* outer_modtypes = nullptr);
static std::vector<o::ValPtr> emit_sig_items(const std::vector<SigItem>& items,
                                             std::map<std::string, bool>& referenced,
                                             int& stamp,
                                             const std::unordered_map<std::string, int>* outer_types,
                                             const std::unordered_map<std::string, int>* outer_modtypes) {
  // Pre-pass: give every item its stamp up front and record the local type
  // names, so a value emitted before/after a type can still cite it by stamp.
  std::vector<int> item_stamp(items.size());
  std::unordered_map<std::string, int> local_types, local_modtypes;
  for (std::size_t i = 0; i < items.size(); ++i) {
    item_stamp[i] = stamp++;
    if (items[i].k == SigItem::Type) local_types[items[i].name] = item_stamp[i];
    if (items[i].k == SigItem::Modtype) local_modtypes[items[i].name] = item_stamp[i];
  }
  // Types visible here = enclosing-scope types overlaid with this level's own
  // (locals shadow).  A nested `module M = struct type t += A end` extending the
  // ENCLOSING file's `type t = ..` resolves `t` through this map to the outer
  // Local ident (printed just `t`); without it the typext degrades to a plain
  // exception.  Passed as the outer scope to nested signatures.
  std::unordered_map<std::string, int> visible;
  if (outer_types) visible = *outer_types;
  for (auto& [n, s] : local_types) visible[n] = s;
  // Module types visible here, same overlay rule -- a `(K : Key)` functor
  // parameter or `module MD5 : S` decl cites its modtype by Local stamp.
  std::unordered_map<std::string, int> visible_mt;
  if (outer_modtypes) visible_mt = *outer_modtypes;
  for (auto& [n, s] : local_modtypes) visible_mt[n] = s;
  // The Path.t for a named modtype reference: a dotted name goes through the
  // head unit's global (importing it), a bare one through the visible map's
  // Local stamp.  Null when the name can't be placed (caller falls back to
  // the inlined signature).
  auto modtype_path = [&](const std::string& ref) -> o::ValPtr {
    if (auto dot = ref.find('.'); dot != std::string::npos) {
      std::string head = global_of(ref.substr(0, dot));
      referenced.emplace(head, true);
      o::ValPtr path = o::vblock(0, {o::vblock(2, {o::vstr(head)})});  // Pident(Global)
      for (std::size_t pos = dot; pos != std::string::npos;) {
        std::size_t nd = ref.find('.', pos + 1);
        path = o::vblock(1, {path, o::vstr(ref.substr(pos + 1,
            nd == std::string::npos ? std::string::npos : nd - pos - 1))});  // Pdot
        pos = nd;
      }
      return path;
    }
    if (auto it = visible_mt.find(ref); it != visible_mt.end())
      return o::vblock(0, {o::vblock(0, {o::vstr(ref), o::vint(it->second)})});  // Pident(Local)
    return nullptr;
  };
  std::vector<o::ValPtr> sig;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const SigItem& it = items[i];
    TyEmit te; te.referenced = &referenced; te.local_types = &visible; te.local_modtypes = &visible_mt;
    auto ident = o::vblock(0, {o::vstr(it.name), o::vint(item_stamp[i])});  // Ident.Local{name;stamp}
    if (it.k == SigItem::Value) {
      o::ValPtr valkind;
      if (it.prim.empty() && it.prim_native.empty()) {
        // `external f : t = "" "native"` has an EMPTY bytecode prim name but is
        // still Val_prim; only a value with neither name is Val_reg.
        valkind = o::vint(0);  // Val_reg
      } else {
        // Val_prim(Primitive.description): an external; inlined by consumers and
        // taking no module field.  prim_native_repr_args length must = arity.
        int arity = 0;
        for (TyPtr a = it.ty; a && a->k == Ty::Arrow; a = a->args[1]) ++arity;
        auto repr_val = [](int c) -> o::ValPtr {
          switch (c) {
            case 1: return o::vint(1);                  // Unboxed_float
            case 2: return o::vint(2);                  // Untagged_immediate
            case 3: return o::vblock(0, {o::vint(1)});  // Unboxed_integer Pint32
            case 4: return o::vblock(0, {o::vint(2)});  // Unboxed_integer Pint64
            case 5: return o::vblock(0, {o::vint(0)});  // Unboxed_integer Pnativeint
            default: return o::vint(0);                 // Same_as_ocaml_repr
          }
        };
        std::vector<o::ValPtr> reprs;
        for (int i = 0; i < arity; ++i)
          reprs.push_back(repr_val(i < (int)it.prim_reprs.size() ? it.prim_reprs[i] : 0));
        auto desc = o::vblock(0, {o::vstr(it.prim), o::vint(arity),
                                  o::vint(it.prim_alloc ? 1 : 0),
                                  o::vstr(it.prim_native),
                                  reprs.empty() ? o::vint(0) : o::vlist(reprs),
                                  repr_val(it.prim_repr_res)});
        valkind = o::vblock(0, {desc});  // Val_prim
      }
      auto vdesc = o::vblock(0, {te.emit(it.ty), valkind, loc_none(),
                                 o::vint(0) /*[] attrs*/, o::vint(0) /*Uid.Internal*/});
      sig.push_back(o::vblock(0, {ident, vdesc, o::vint(0) /*Exported*/}));  // Sig_value
    } else if (it.k == SigItem::Module) {
      // Sig_module(id, Mp_present, module_declaration, rec_status, visibility).
      // A submodule takes a runtime field, so it must appear in the signature
      // to keep the surrounding value field layout aligned.
      o::ValPtr mty;
      int presence = 0;  // Mp_present: takes a runtime field
      if (it.is_functor) {
        // Mty_functor(Named(Some p1, ..), Mty_functor(Named(Some p2, ..), ..
        // Mty_signature(result))): the curried parameter chain, innermost last.
        // A generative parameter is Unit (the int constructor 0); otherwise
        // Named(Some id, <param sig>).  The functor takes a runtime field.
        auto mk_param = [&](bool unit, const std::string& pname,
                            const std::vector<SigItem>& psig_items,
                            const std::string& ref) -> o::ValPtr {
          if (unit) return o::vint(0);  // functor_parameter = Unit
          auto pident = o::vblock(0, {o::vstr(pname), o::vint(stamp++)});  // Ident.Local
          // A NAMED param modtype (`(K : Key)`) emits Mty_ident(Key) like
          // ocamlc; the inlined signature is the fallback.
          o::ValPtr psig;
          if (!ref.empty())
            if (o::ValPtr mp = modtype_path(ref)) psig = o::vblock(0, {mp});  // Mty_ident
          if (!psig)
            psig = o::vblock(1, {o::vlist(emit_sig_items(psig_items, referenced, stamp, &visible, &visible_mt))});  // Mty_signature
          return o::vblock(0, {o::vblock(0, {pident}) /*Some*/, psig});  // Named(Some, <param sig>)
        };
        std::vector<o::ValPtr> params;
        params.push_back(mk_param(it.functor_unit, it.functor_param, it.param_sig,
                                  it.functor_param_ref));
        for (std::size_t p = 0; p < it.more_param_names.size(); ++p)
          params.push_back(mk_param(p < it.more_param_units.size() && it.more_param_units[p],
                                    it.more_param_names[p],
                                    p < it.more_param_sigs.size() ? it.more_param_sigs[p]
                                                                  : std::vector<SigItem>{},
                                    p < it.more_param_refs.size() ? it.more_param_refs[p]
                                                                  : std::string()));
        o::ValPtr body = o::vblock(1, {o::vlist(emit_sig_items(it.sub, referenced, stamp, &visible, &visible_mt))});  // Mty_signature(result)
        for (auto p = params.rbegin(); p != params.rend(); ++p)
          body = o::vblock(2, {*p, body});  // Mty_functor
        mty = body;
      } else if (!it.alias.empty()) {
        // `module name = <target>`: Mty_alias(path), Mp_absent -- an alias is
        // transparent and takes NO runtime field.  A single-component target is
        // Pident(Global unit); a dotted target (`Uid = Shape.Uid`) is
        // Pdot(Pident(Global head), comp..).  Record the HEAD unit as imported.
        size_t dot = it.alias.find('.');
        std::string head = dot == std::string::npos ? it.alias : it.alias.substr(0, dot);
        referenced.emplace(head, false);
        o::ValPtr path = o::vblock(0, {o::vblock(2, {o::vstr(head)})});  // Pident(Global head)
        for (size_t pos = dot; pos != std::string::npos;) {
          size_t nd = it.alias.find('.', pos + 1);
          std::string comp = it.alias.substr(pos + 1,
              nd == std::string::npos ? std::string::npos : nd - pos - 1);
          path = o::vblock(1, {path, o::vstr(comp)});  // Pdot(path, comp)
          pos = nd;
        }
        mty = o::vblock(3, {path});  // Mty_alias
        presence = 1;  // Mp_absent
      } else {
        // `module MD5 : S` (a NAMED modtype) emits Mty_ident(S) like ocamlc;
        // the inlined signature is the fallback.
        if (!it.modtype_ref.empty())
          if (o::ValPtr mp = modtype_path(it.modtype_ref)) mty = o::vblock(0, {mp});  // Mty_ident
        if (!mty)
          mty = o::vblock(1, {o::vlist(emit_sig_items(it.sub, referenced, stamp, &visible, &visible_mt))});  // Mty_signature
      }
      auto md = o::vblock(0, {mty, o::vint(0) /*[] attrs*/, loc_none(),
                              o::vint(0) /*md_uid*/});  // module_declaration
      sig.push_back(o::vblock(3, {ident, o::vint(presence), md,
                                  o::vint(it.rec_status) /*Trec_*/,
                                  o::vint(0) /*Exported*/}));  // Sig_module
    } else if (it.k == SigItem::Modtype) {
      // Sig_modtype(id, modtype_declaration, vis).  mtd_type = Some(Mty_signature
      // sig).  Takes NO runtime field, so it never shifts the value layout.
      auto msig = o::vblock(1, {o::vlist(emit_sig_items(it.sub, referenced, stamp, &visible, &visible_mt))});  // Mty_signature
      auto mtd = o::vblock(0, {o::vblock(0, {msig}) /*Some*/, o::vint(0) /*attrs*/,
                               loc_none(), o::vint(0) /*mtd_uid*/});  // modtype_declaration
      sig.push_back(o::vblock(4, {ident, mtd, o::vint(0) /*Exported*/}));  // Sig_modtype
    } else if (it.k == SigItem::Exception) {
      // Sig_typext(id, extension_constructor, ext_status, vis).  A plain
      // exception extends the predefined `exn` (Text_exception); a `type t +=`
      // extension constructor carries the extended type's path, its declared
      // params (fresh vars, printed `_`), a GADT return type when written
      // (`E : unit Effect.t`), and Text_first/Text_next so ocamlc prints the
      // group as one `type t += A | B`.  Both TAKE a runtime field.
      o::ValPtr path;
      const std::vector<std::string>* eparams = nullptr;
      int status = 2;  // Text_exception
      if (!it.ext_path.empty()) {
        path = te.type_path(it.ext_path);
        if (path) { eparams = &it.ext_params; status = it.text_kind; }
        // an unplaceable extended type degrades to a plain exception (valid)
      }
      if (!path)
        path = o::vblock(0, {o::vblock(3, {o::vstr("exn"), o::vint(7)})});  // Pident(Predef exn)
      o::ValPtr cargs;
      if (!it.ctors.empty() && !it.ctors[0].inline_record.empty()) {
        // Cstr_record inline-record payload (`exception E of {l;..}`): emit the
        // label_declarations so a consumer matching `M.E {l = ..}` resolves the
        // labels (without this, ext_match bails and the whole match collapses).
        int lstamp = 290;
        std::vector<o::ValPtr> lds;
        for (auto& l : it.ctors[0].inline_record) {
          auto lid = o::vblock(0, {o::vstr(l.name), o::vint(lstamp++)});  // ld_id
          lds.push_back(o::vblock(0, {lid, o::vint(l.mut ? 1 : 0) /*ld_mutable*/,
                                      o::vint(l.atomic ? 1 : 0) /*ld_atomic*/, te.emit(l.ty),
                                      loc_none(), o::vint(0) /*attrs*/, o::vint(0) /*Uid*/}));
        }
        cargs = o::vblock(1, {o::vlist(lds)});  // Cstr_record
      } else {
        std::vector<o::ValPtr> args;
        if (!it.ctors.empty()) for (auto& a : it.ctors[0].args) args.push_back(te.emit(a));
        cargs = o::vblock(0, {args.empty() ? o::vint(0) : o::vlist(args)});  // Cstr_tuple
      }
      std::vector<o::ValPtr> tparams;
      if (eparams)
        for (const std::string& pn : *eparams)  // Tvar(Some source-name), e.g. "_"
          tparams.push_back(te.texpr(o::vblock(0, {o::vblock(0, {o::vstr(pn)})})));
      auto ret = it.ext_ret ? o::vblock(0, {te.emit(it.ext_ret)}) : o::vint(0);  // Some/None
      auto extcon = o::vblock(0, {path,
                                  tparams.empty() ? o::vint(0) : o::vlist(tparams),  // ext_type_params
                                  cargs, ret,
                                  o::vint(1) /*ext_private Public*/,
                                  loc_none(), o::vint(0) /*ext_attributes*/, o::vint(0) /*ext_uid*/});
      sig.push_back(o::vblock(2, {ident, extcon, o::vint(status),
                                  o::vint(0) /*Exported*/}));  // Sig_typext
    } else {
      // type_declaration (14 fields).  Type_abstract kind; a manifest makes it an
      // alias (`type t = manifest`).  Variant/record kinds: the climb.
      std::vector<o::ValPtr> ps;
      for (auto& p : it.params) ps.push_back(te.emit(p));
      auto man = it.manifest ? o::vblock(0, {te.emit(it.manifest)}) : o::vint(0);  // Some/None
      o::ValPtr kind;
      if (!it.ctors.empty()) {
        int cstamp = 270;
        std::vector<o::ValPtr> cds;
        for (auto& c : it.ctors) {
          auto cid = o::vblock(0, {o::vstr(c.name), o::vint(cstamp++)});  // cd_id = Ident.Local
          o::ValPtr cargs;
          if (!c.inline_record.empty()) {  // Cstr_record of label_declaration list
            int lstamp = 290;
            std::vector<o::ValPtr> lds;
            for (auto& l : c.inline_record) {
              auto lid = o::vblock(0, {o::vstr(l.name), o::vint(lstamp++)});  // ld_id
              lds.push_back(o::vblock(0, {lid, o::vint(l.mut ? 1 : 0) /*ld_mutable*/,
                                          o::vint(l.atomic ? 1 : 0) /*ld_atomic*/, te.emit(l.ty),
                                          loc_none(), o::vint(0) /*attrs*/, o::vint(0) /*Uid*/}));
            }
            cargs = o::vblock(1, {o::vlist(lds)});  // Cstr_record
          } else {
            std::vector<o::ValPtr> args;
            for (auto& a : c.args) args.push_back(te.emit(a));
            cargs = o::vblock(0, {args.empty() ? o::vint(0) : o::vlist(args)});  // Cstr_tuple
          }
          auto cres = c.res ? o::vblock(0, {te.emit(c.res)}) : o::vint(0);  // cd_res Some/None
          cds.push_back(o::vblock(0, {cid, cargs, cres, loc_none(),
                                      o::vint(0) /*attrs*/, o::vint(0) /*Uid.Internal*/}));
        }
        // variant_representation: Variant_regular (0) or Variant_unboxed (1),
        // the latter for a single single-field ctor marked `[@@unboxed]`.
        kind = o::vblock(2, {o::vlist(cds), o::vint(it.type_unboxed ? 1 : 0)});  // Type_variant
      } else if (!it.labels.empty()) {
        int lstamp = 280;
        std::vector<o::ValPtr> lds;
        for (auto& l : it.labels) {
          auto lid = o::vblock(0, {o::vstr(l.name), o::vint(lstamp++)});  // ld_id
          lds.push_back(o::vblock(0, {lid, o::vint(l.mut ? 1 : 0) /*ld_mutable*/,
                                      o::vint(l.atomic ? 1 : 0) /*ld_atomic*/, te.emit(l.ty),
                                      loc_none(), o::vint(0) /*attrs*/, o::vint(0) /*Uid*/}));
        }
        // record_representation: Record_regular (const 0) or, for a single-field
        // `[@@unboxed]` record, Record_unboxed of bool (block tag 0; false = not
        // an inlined record).
        auto rep = it.type_unboxed ? o::vblock(0, {o::vint(0)}) : o::vint(0);
        kind = o::vblock(1, {o::vlist(lds), rep});  // Type_record
      } else if (it.type_open) {
        kind = o::vint(0);  // Type_open (`type t = ..`), the lone constant ctor
      } else {
        kind = o::vblock(0, {o::vint(0)});  // Type_abstract(Definition)
      }
      auto tdecl = o::vblock(0, {
          ps.empty() ? o::vint(0) : o::vlist(ps),     // type_params
          o::vint((long long)it.params.size()),       // type_arity
          kind,                                        // type_kind
          o::vint(it.type_private ? 0 : 1),            // type_private (0 Private / 1 Public)
          man,                                         // type_manifest
          // type_variance / type_separability: ONE entry per parameter (OCaml
          // iter2's them against the params -- a length mismatch aborts).
          // Variance = Variance.unknown (May_pos|May_neg = 1|6 = 7), the value
          // OCaml assigns a param whose variance it can't derive.  Printtyp shows
          // variance when with_variance is true (abstract, private, or GADT decls:
          // out_type.ml `abstr`): unknown's get_upper = (true,true) prints as
          // NoVariance (nothing), matching the oracle -- whereas null (0) would
          // print Bivariant `+-` on every GADT param.  Concrete non-private decls
          // set with_variance=false, so the stored value is not shown either way.
          [&] { std::vector<o::ValPtr> v(it.params.size(), o::vint(7)); return v.empty() ? o::vint(0) : o::vlist(v); }(),
          [&] { std::vector<o::ValPtr> v(it.params.size(), o::vint(0)); return v.empty() ? o::vint(0) : o::vlist(v); }(),
          o::vint(0), o::vint(0),                      // is_newtype false, expansion_scope 0
          loc_none(),                                  // type_loc
          // type_attributes: Printtyp derives the printed `[@@immediate]` /
          // `[@@immediate64]` from Type_immediacy.of_attributes of THIS field (not
          // from type_immediate), so emit the real attribute when marked.
          [&]() -> o::ValPtr {
            if (!it.type_immediate) return o::vint(0);  // []
            const char* nm = it.type_immediate == 2 ? "immediate64" : "immediate";
            auto attr = o::vblock(0, {
                o::vblock(0, {o::vstr(nm), loc_none()}),  // attr_name : string loc
                o::vblock(0, {o::vint(0)}),               // attr_payload = PStr []
                loc_none()});                             // attr_loc
            return o::vlist({attr});
          }(),
          o::vint(it.type_immediate), o::vint(0),      // type_immediate, unboxed false
          o::vint(0)});                                // type_uid = Uid.Internal
      sig.push_back(o::vblock(1, {ident, tdecl,
                                  // Trec_first, or Trec_next for the `and`
                                  // members of a mutually-recursive group
                                  o::vint(it.rec_status ? it.rec_status : 1),
                                  o::vint(0) /*Exported*/}));
    }
  }
  return sig;
}

std::string write_cmi(const std::string& path, const std::string& modname,
                      const std::vector<SigItem>& items,
                      const std::vector<Import>& imports) {
  std::map<std::string, bool> referenced;  // cited global unit -> needs real CRC
  int stamp = 300;
  auto header = o::vblock(0, {o::vstr(modname), o::vlist(emit_sig_items(items, referenced, stamp))});
  std::vector<std::uint8_t> hbytes = o::marshal(header);

  const std::string MAGIC = "Caml1999I038";
  std::string prefix(MAGIC);
  prefix.append(reinterpret_cast<const char*>(hbytes.data()), hbytes.size());
  std::string self_crc = blake2::blake128(reinterpret_cast<const unsigned char*>(prefix.data()),
                                          prefix.size());

  auto crc_opt = [](const std::string& c) { return o::vblock(0, {o::vstr(c)}); };  // Some
  std::vector<o::ValPtr> crcs = {o::vblock(0, {o::vstr(modname), crc_opt(self_crc)})};
  std::set<std::string> seen = {modname};
  for (auto& im : imports) {
    if (!seen.insert(im.name).second) continue;
    crcs.push_back(o::vblock(0, {o::vstr(im.name), crc_opt(im.crc)}));
  }
  // Units cited by the signature.  A qualified type (`Buffer.t`) is imported
  // with the unit's real CRC (read from its .cmi) so the interface stays
  // self-consistent; a module alias (`module M = Unit`) is imported with
  // CRC=None -- like ocamlc's -no-alias-deps -- so stdlib.cmi can reference
  // Stdlib__List without Stdlib__List (built against stdlib.cmi) creating a
  // circular CRC dependency.
  for (const auto& [g, wants_crc] : referenced) {
    if (!seen.insert(g).second) continue;
    if (wants_crc) {
      std::string crc = read_cmi_self_crc(resolve_cmi_global(g));
      if (crc.empty()) continue;  // can't locate it -> omit (still valid)
      crcs.push_back(o::vblock(0, {o::vstr(g), crc_opt(crc)}));
    } else {
      crcs.push_back(o::vblock(0, {o::vstr(g), o::vint(0) /*CRC None*/}));
    }
  }
  std::vector<std::uint8_t> cbytes = o::marshal(o::vlist(crcs));

  // flags = [Alerts <empty map>]  (Alerts is the lone single-arg pers_flags ctor)
  std::vector<std::uint8_t> fbytes = o::marshal(o::vlist({o::vblock(0, {o::vint(0)})}));

  std::ofstream out(path, std::ios::binary);
  out.write(prefix.data(), prefix.size());
  out.write(reinterpret_cast<const char*>(cbytes.data()), cbytes.size());
  out.write(reinterpret_cast<const char*>(fbytes.data()), fbytes.size());
  return self_crc;
}

void set_module_dirs(const std::string& stdlib_dir,
                     const std::vector<std::string>& dirs) {
  g_stdlib_dir = stdlib_dir;
  g_module_dirs = dirs;
}

std::string write_cmi(const std::string& path, const std::string& modname,
                      const std::vector<std::pair<std::string, TyPtr>>& values,
                      const std::vector<Import>& imports) {
  std::vector<SigItem> items;
  for (auto& [n, t] : values) items.push_back(sig_value(n, t));
  return write_cmi(path, modname, items, imports);
}

namespace {
// Rebuild a decoded Marshal value as an omarshal graph (for reusing a member
// .cmi's signature blob verbatim).  Mirrors link.cpp's conv.
o::ValPtr conv_value(const m::Arena& a, std::size_t id) {
  const m::Value& v = a[id];
  switch (v.kind) {
    case m::Value::Kind::Int:
      if (!v.custom_raw.empty())
        return o::vcustom(v.custom_raw, 1 + (v.custom_bsize + 7) / 8);
      return o::vint(v.i);
    case m::Value::Kind::String: return o::vstr(v.str);
    case m::Value::Kind::Double: return o::vdbl(v.d);
    case m::Value::Kind::Block: {
      std::vector<o::ValPtr> fs;
      for (auto f : v.fields) fs.push_back(conv_value(a, f));
      return o::vblock((int)v.tag, std::move(fs));
    }
    case m::Value::Kind::DoubleArray: return o::vdblarr(v.darr);
  }
  return o::vint(0);
}

// Read a .cmi file's marshaled header `(name, signature)` and return the
// signature as an omarshal graph; `*out_name` receives the stored module name.
o::ValPtr read_cmi_sign(const std::string& path, std::string* out_name) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  std::size_t off = 0;
  for (; off + 4 <= bytes.size(); ++off)
    if (bytes[off] == 0x84 && bytes[off + 1] == 0x95 && bytes[off + 2] == 0xA6 &&
        (bytes[off + 3] == 0xBE || bytes[off + 3] == 0xBF || bytes[off + 3] == 0xBD))
      break;
  if (off + 4 > bytes.size()) throw std::runtime_error("no marshal header in " + path);
  m::Arena arena;
  std::size_t hid = m::read_value(bytes.data(), bytes.size(), off, arena);  // (name, sign)
  const m::Value& hv = arena[hid];
  if (hv.kind != m::Value::Kind::Block || hv.fields.size() < 2)
    throw std::runtime_error("malformed cmi header in " + path);
  if (out_name) *out_name = arena[hv.fields[0]].str;
  return conv_value(arena, hv.fields[1]);
}
}  // namespace

std::string write_packed_cmi(const std::string& path, const std::string& pack_name,
                             const std::vector<std::string>& member_cmis) {
  int stamp = 200;
  std::vector<o::ValPtr> sig_items;
  for (const auto& mc : member_cmis) {
    std::string mname;
    o::ValPtr member_sign = read_cmi_sign(mc, &mname);
    // module <Member> : sig <member_sign> end
    auto ident = o::vblock(0, {o::vstr(mname), o::vint(stamp++)});       // Ident.Local
    auto mty = o::vblock(1, {member_sign});                             // Mty_signature
    auto md = o::vblock(0, {mty, o::vint(0) /*[] attrs*/, loc_none(),
                            o::vint(0) /*md_uid*/});                    // module_declaration
    sig_items.push_back(o::vblock(3, {ident, o::vint(0) /*Mp_present*/, md,
                                      o::vint(0) /*Trec_not*/, o::vint(0) /*Exported*/}));
  }

  auto header = o::vblock(0, {o::vstr(pack_name), o::vlist(sig_items)});
  std::vector<std::uint8_t> hbytes = o::marshal(header);

  const std::string MAGIC = "Caml1999I038";
  std::string prefix(MAGIC);
  prefix.append(reinterpret_cast<const char*>(hbytes.data()), hbytes.size());
  std::string self_crc = blake2::blake128(reinterpret_cast<const unsigned char*>(prefix.data()),
                                          prefix.size());

  auto crc_opt = [](const std::string& c) { return o::vblock(0, {o::vstr(c)}); };  // Some
  std::vector<o::ValPtr> crcs = {o::vblock(0, {o::vstr(pack_name), crc_opt(self_crc)})};
  std::vector<std::uint8_t> cbytes = o::marshal(o::vlist(crcs));

  // flags = [Alerts <empty map>]
  std::vector<std::uint8_t> fbytes = o::marshal(o::vlist({o::vblock(0, {o::vint(0)})}));

  std::ofstream out(path, std::ios::binary);
  out.write(prefix.data(), prefix.size());
  out.write(reinterpret_cast<const char*>(cbytes.data()), cbytes.size());
  out.write(reinterpret_cast<const char*>(fbytes.data()), fbytes.size());
  return self_crc;
}

}  // namespace cmiw

}  // namespace cppcaml::cmi
