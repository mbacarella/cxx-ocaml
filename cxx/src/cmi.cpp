#include "cppcaml/cmi.hpp"
#include "cppcaml/dbgenv.hpp"
#include "cppcaml/omarshal.hpp"
#include "cppcaml/blake2.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

// Region-contained cmi decode (the mmap cmi cache) needs the mimalloc arena/
// heap API and mmap/mprotect; without them load() just always decodes normally.
#include <cstdint>
#if defined(CPPCAML_HAVE_MIMALLOC) && defined(__linux__) && \
    UINTPTR_MAX > 0xffffffffu  // fixed VA slots need a 64-bit address space
#define CPPCAML_CMI_REGIONS 1
#include <mimalloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>   // dl_iterate_phdr: our own GNU build-id
#include <climits>
#include <cstring>
#endif

namespace cppcaml::cmi {

namespace {

// Process-lifetime bump arena for the decoded cmi graph (TypeExpr / Path /
// ModuleType / Signature nodes).  Nodes are never freed: a decoded .cmi lives
// in the load cache for the whole process (the compiler fast-exits, skipping
// teardown), so there is no ownership to track -- GraphPtr handles are plain
// pointers.  Slabs never move, so raw node pointers stay valid.  Slab memory
// comes from plain operator new so a region-scoped heap (the mmap cmi cache)
// can later redirect where slabs land.
struct GraphArena {
  static constexpr std::size_t kSlab = 1 << 18;  // 256KB
  char* cur_ = nullptr;       // current slab; past slabs are intentionally
                              // leaked (nodes live for the whole process)
  std::size_t used_ = kSlab;  // == kSlab forces a fresh slab on first alloc
  void* alloc(std::size_t n, std::size_t a) {
    used_ = (used_ + a - 1) & ~(a - 1);
    if (used_ + n > kSlab) {
      cur_ = static_cast<char*>(::operator new(kSlab));
      used_ = 0;
    }
    void* p = cur_ + used_;
    used_ += n;
    return p;
  }
  // Abandon the current slab's tail: the next alloc starts a fresh slab from
  // whatever heap is then active.  Called around a region-contained decode so
  // one slab never holds nodes of two different regions (a node written into
  // an earlier, already-sealed region would fault on its PROT_READ pages).
  void fresh() { used_ = kSlab; }
};
GraphArena g_graph_arena;

}  // namespace

TypePtr type_alloc() {
  return TypePtr{new (g_graph_arena.alloc(sizeof(TypeExpr), alignof(TypeExpr)))
                     TypeExpr()};
}
PathPtr path_alloc() {
  return PathPtr{new (g_graph_arena.alloc(sizeof(Path), alignof(Path))) Path()};
}
ModuleTypePtr modtype_alloc() {
  return ModuleTypePtr{
      new (g_graph_arena.alloc(sizeof(ModuleType), alignof(ModuleType)))
          ModuleType()};
}
SignaturePtr sig_alloc(Signature&& s) {
  return SignaturePtr{
      new (g_graph_arena.alloc(sizeof(Signature), alignof(Signature)))
          Signature(std::move(s))};
}

namespace {

namespace m = marshal;

// Slurp an open binary stream into a byte vector in one bulk read (size via
// seek, then a single read), instead of istreambuf_iterator's byte-at-a-time
// push_back that reallocs-and-moves the buffer repeatedly.  Runs once per .cmi
// decode (75+ per compile), so it showed up as a vector<uint8_t> realloc hotspot.
inline std::vector<std::uint8_t> slurp_bytes(std::ifstream& in) {
  in.seekg(0, std::ios::end);
  std::streampos sz = in.tellg();
  in.seekg(0, std::ios::beg);
  if (sz <= 0)  // non-seekable/empty: fall back to the iterator slurp
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sz));
  in.read(reinterpret_cast<char*>(bytes.data()), sz);
  bytes.resize(static_cast<std::size_t>(in.gcount()));
  return bytes;
}

// Walks a decoded Marshal arena and reconstructs Types structures on demand.
class Decoder {
public:
  // The memo is preallocated dense (arena ids index it directly) at
  // construction time, so decoding performs no memo allocation: during a
  // region-contained decode the Decoder is constructed BEFORE the region heap
  // takes over, keeping this purely-transient table out of the sealed dump.
  explicit Decoder(const m::Arena& arena, int cmi_id = 0)
      : arena_(arena), memo_(arena.size()), path_prov_(arena.size(), 0),
        cmi_id_(cmi_id) {}

  // type_expr is the transient_expr record { desc; level; scope; id };
  // field 0 is the type_desc.  Memoize by arena id so shared/cyclic graphs
  // map to shared/cyclic C++ nodes.
  TypePtr type(std::size_t id) {
    if (TypePtr t = memo_[id]) return t;
    TypePtr t = type_alloc();
    memo_[id] = t;
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
              // value_description: field 0 = val_type, field 1 = val_kind,
              // field 2 = val_loc.
              const m::Value& vd = arena_[item.fields[1]];
              sv.type = type(vd.fields.at(0));
              if (vd.fields.size() > 2) sv.loc = decode_loc(vd.fields[2]);
              if (vd.fields.size() > 4) sv.uid = decode_uid(vd.fields[4]);
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
                  // description record: {prim_name; prim_arity; prim_alloc;
                  // prim_native_name; prim_native_repr_args; prim_native_repr_res}
                  if (!desc.fields.empty()) sv.prim = arena_[desc.fields[0]].str();
                  if (desc.fields.size() > 1 &&
                      arena_[desc.fields[1]].kind == m::Value::Kind::Int)
                    sv.prim_arity = (int)arena_[desc.fields[1]].i;
                  // native_repr -> the writer's code: Same_as=0, Unboxed_float=1,
                  // Untagged_immediate=2, Unboxed_integer Pint32/Pint64/
                  // Pnativeint = 3/4/5 (inverse of TyEmit's repr_val).
                  auto repr_code = [&](const m::Value& v) -> int {
                    if (v.kind == m::Value::Kind::Int) return (int)v.i;
                    if (!v.fields.empty() &&
                        arena_[v.fields[0]].kind == m::Value::Kind::Int)
                      switch (arena_[v.fields[0]].i) {
                        case 1: return 3;
                        case 2: return 4;
                        default: return 5;
                      }
                    return 0;
                  };
                  if (desc.fields.size() > 2 &&
                      arena_[desc.fields[2]].kind == m::Value::Kind::Int)
                    sv.prim_alloc = arena_[desc.fields[2]].i != 0;
                  if (desc.fields.size() > 3)
                    sv.prim_native = arena_[desc.fields[3]].str();
                  if (desc.fields.size() > 4)
                    for (std::size_t c = desc.fields[4];
                         arena_[c].kind == m::Value::Kind::Block &&
                         arena_[c].fields.size() == 2;
                         c = arena_[c].fields[1])
                      sv.prim_reprs.push_back(repr_code(arena_[arena_[c].fields[0]]));
                  if (desc.fields.size() > 5)
                    sv.prim_repr_res = repr_code(arena_[desc.fields[5]]);
                }
              }
              out.values.push_back(std::move(sv));
            }
            break;
          case 1:  // Sig_type of Ident.t * type_declaration * rec_status * vis
            if (item.fields.size() == 4) {
              TypeDecl td = type_declaration(item.fields[1]);
              td.name = ident(item.fields[0]).name;
              td.stamp = ident(item.fields[0]).stamp;
              if (arena_[item.fields[2]].kind == m::Value::Kind::Int)
                td.rec_status = (int)arena_[item.fields[2]].i;
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
              // module_declaration: md_type(0), md_attributes(1), md_loc(2).
              md.type = module_type(arena_[item.fields[2]].fields.at(0));
              if (arena_[item.fields[2]].fields.size() > 2)
                md.loc = decode_loc(arena_[item.fields[2]].fields[2]);
              if (arena_[item.fields[2]].fields.size() > 3)
                md.uid = decode_uid(arena_[item.fields[2]].fields[3]);
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
              // modtype_declaration: mtd_type(0) option, mtd_attributes(1),
              // mtd_loc(2).
              const m::Value& opt = arena_[arena_[item.fields[1]].fields.at(0)];
              if (opt.kind == m::Value::Kind::Block && opt.tag == 0)
                mtd.type = module_type(opt.fields.at(0));
              if (arena_[item.fields[1]].fields.size() > 2)
                mtd.loc = decode_loc(arena_[item.fields[1]].fields[2]);
              if (arena_[item.fields[1]].fields.size() > 3)
                mtd.uid = decode_uid(arena_[item.fields[1]].fields[3]);
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

  // Like signature(), but decodes ONLY top-level Sig_type items into out.types
  // -- no value type-graphs (Sig_value), no recursion into submodule signatures
  // (Sig_module -> module_type -> signature).  Those two are the bulk of a full
  // decode; the labelset index needs only the records/variants this leaves.
  Signature signature_types_only(std::size_t list_id) {
    Signature out;
    for (std::size_t cur = list_id; arena_[cur].kind == m::Value::Kind::Block &&
                                    !arena_[cur].fields.empty();) {
      const m::Value& cons = arena_[cur];
      const m::Value& item = arena_[cons.fields[0]];
      if (item.kind == m::Value::Kind::Block && item.tag == 1 &&
          item.fields.size() == 4) {  // Sig_type of Ident * type_declaration * ..
        TypeDecl td = type_declaration(item.fields[1]);
        td.name = ident(item.fields[0]).name;
        td.stamp = ident(item.fields[0]).stamp;
        out.types.push_back(std::move(td));
      }
      cur = cons.fields[1];  // tail
    }
    return out;
  }

  // module_type (typing/types.mli): Mty_ident / Mty_signature / Mty_functor /
  // Mty_alias.
  ModuleTypePtr module_type(std::size_t id) {
    const m::Value& v = arena_[id];
    ModuleTypePtr mt = modtype_alloc();
    switch (v.tag) {
      case 0:  // Mty_ident of Path.t
        mt->kind = ModuleType::Ident;
        mt->path = path(v.fields.at(0));
        break;
      case 1:  // Mty_signature of signature
        mt->kind = ModuleType::Sig;
        mt->sig = sig_alloc(signature(v.fields.at(0)));
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
    if (v.fields.size() > 7) ec.uid = decode_uid(v.fields[7]);
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
    // type_private : private_flag (field 3; 0 Private / 1 Public).
    if (arena_[d.fields.at(3)].kind == m::Value::Kind::Int)
      td.priv = arena_[d.fields[3]].i == 0;
    // type_manifest : type_expr option (field 4).
    const m::Value& man = arena_[d.fields.at(4)];
    if (man.kind == m::Value::Kind::Block && man.tag == 0)
      td.manifest = type(man.fields.at(0));
    // type_variance : Variance.t list (field 5) -- raw ints, one per param.
    if (d.fields.size() > 5)
      for (std::size_t cur = d.fields[5];
           arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
        const m::Value& cons = arena_[cur];
        if (arena_[cons.fields.at(0)].kind == m::Value::Kind::Int)
          td.variances.push_back(arena_[cons.fields[0]].i);
        cur = cons.fields[1];
      }
    // type_separability : Separability.t list (field 6) -- constant ctors.
    if (d.fields.size() > 6)
      for (std::size_t cur = d.fields[6];
           arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();) {
        const m::Value& cons = arena_[cur];
        if (arena_[cons.fields.at(0)].kind == m::Value::Kind::Int)
          td.separability.push_back(static_cast<int>(arena_[cons.fields[0]].i));
        cur = cons.fields[1];
      }
    if (d.fields.size() > 9) td.loc = decode_loc(d.fields[9]);  // type_loc
    // type_immediate : Type_immediacy.t (field 11; all-constant ctors are
    // constant, so it marshals as an int).
    if (d.fields.size() > 11 && arena_[d.fields[11]].kind == m::Value::Kind::Int)
      td.immediate = static_cast<int>(arena_[d.fields[11]].i);
    // type_unboxed_default : bool (field 12).
    if (d.fields.size() > 12 && arena_[d.fields[12]].kind == m::Value::Kind::Int)
      td.unboxed_default = arena_[d.fields[12]].i == 1;
    if (d.fields.size() > 13) td.uid = decode_uid(d.fields[13]);
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
        // record_representation: Record_regular(0)/Record_float(1) are constants;
        // Record_unboxed of bool is block tag 0 (a single-field unboxed record).
        if (k.fields.size() > 1) {
          const m::Value& rep = arena_[k.fields[1]];
          td.unboxed = rep.kind == m::Value::Kind::Block && rep.tag == 0;
          td.record_float = rep.kind == m::Value::Kind::Int && rep.i == 1;
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
        // variant_representation: Variant_regular(0)/Variant_unboxed(1) are both
        // constant constructors, so unboxed <=> the repr int is 1.
        if (k.fields.size() > 1) {
          const m::Value& rep = arena_[k.fields[1]];
          td.unboxed = rep.kind == m::Value::Kind::Int && rep.i == 1;
        }
        break;
      case 3:  // Type_external of string
        td.kind = TypeDecl::External;
        td.external_name = arena_[k.fields.at(0)].str();
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
    ld.stamp = ident(v.fields.at(0)).stamp;
    ld.mutable_ = arena_[v.fields.at(1)].kind == m::Value::Kind::Int &&
                  arena_[v.fields[1]].i != 0;  // mutable_flag: Mutable = 1
    ld.type = type(v.fields.at(3));
    if (v.fields.size() > 4) ld.loc = decode_loc(v.fields[4]);  // ld_loc
    if (v.fields.size() > 6) ld.uid = decode_uid(v.fields[6]);  // ld_uid
    return ld;
  }

  ConstructorDecl ctor_decl(std::size_t id) {
    const m::Value& v = arena_[id];  // { cd_id; cd_args; cd_res; ... }
    ConstructorDecl cd;
    cd.name = ident(v.fields.at(0)).name;
    cd.stamp = ident(v.fields.at(0)).stamp;
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
    if (v.fields.size() > 3) cd.loc = decode_loc(v.fields[3]);  // cd_loc
    if (v.fields.size() > 5) cd.uid = decode_uid(v.fields[5]);  // cd_uid
    return cd;
  }

  // Shape.Uid.t (typing/shape.ml).  The immediate 0 is Internal; the blocks
  // are Compilation_unit(0) of string, Item(1) of {comp_unit; id; from},
  // Local_opaque_item(2) and Predef(3) of string, with `from` =
  // Unit_info.intf_or_impl = Intf(0) | Impl(1).  A declaration this unit
  // copies keeps the uid decoded here (S564).
  RUid decode_uid(std::size_t id) {
    RUid u;
    const m::Value& v = arena_[id];
    if (v.kind == m::Value::Kind::Int) { u.kind = 4; return u; }  // Internal
    u.kind = static_cast<int>(v.tag);
    if (v.tag == 1 && v.fields.size() >= 3) {
      u.unit = arena_[v.fields[0]].str();
      if (arena_[v.fields[1]].kind == m::Value::Kind::Int)
        u.id = static_cast<int>(arena_[v.fields[1]].i);
      u.intf = arena_[v.fields[2]].kind == m::Value::Kind::Int &&
               arena_[v.fields[2]].i == 0;
    } else if ((v.tag == 0 || v.tag == 3) && !v.fields.empty()) {
      u.unit = arena_[v.fields[0]].str();
    }
    return u;
  }

  // Location.t = { loc_start; loc_end; loc_ghost }; each position is
  // { pos_fname; pos_lnum; pos_bol; pos_cnum }.  Decoded so a spliced member
  // re-emits the dependency's original source location (pos_fname included).
  RLoc decode_loc(std::size_t loc_id) {
    RLoc r;
    const m::Value& loc = arena_[loc_id];
    if (loc.kind != m::Value::Kind::Block || loc.fields.size() < 3) return r;
    const m::Value& g = arena_[loc.fields[2]];  // loc_ghost
    if (g.kind == m::Value::Kind::Int && g.i != 0) return r;  // ghost -> default
    auto rdpos = [&](std::size_t pid, std::string& fn, int& l, int& b, int& c) {
      const m::Value& p = arena_[pid];
      if (p.kind != m::Value::Kind::Block || p.fields.size() < 4) return;
      fn = arena_[p.fields[0]].str();
      if (arena_[p.fields[1]].kind == m::Value::Kind::Int) l = (int)arena_[p.fields[1]].i;
      if (arena_[p.fields[2]].kind == m::Value::Kind::Int) b = (int)arena_[p.fields[2]].i;
      if (arena_[p.fields[3]].kind == m::Value::Kind::Int) c = (int)arena_[p.fields[3]].i;
    };
    std::string fe;
    rdpos(loc.fields[0], r.fname, r.l_s, r.b_s, r.c_s);
    rdpos(loc.fields[1], fe, r.l_e, r.b_e, r.c_e);
    r.ghost = false;
    return r;
  }

private:
  Ident ident(std::size_t id) {
    const m::Value& v = arena_[id];
    Ident out;
    out.kind = static_cast<Ident::Kind>(v.tag);
    if (!v.fields.empty()) out.name = arena_[v.fields[0]].str();
    if (v.fields.size() > 1 && arena_[v.fields[1]].kind == m::Value::Kind::Int)
      out.stamp = arena_[v.fields[1]].i;
    return out;
  }

  // The arena index of a path's head Pident block (a Pdot chain's siblings
  // share it: prefix_idents built them all over ONE root).
  int head_blk(std::size_t id) {
    const m::Value* v = &arena_[id];
    while (v->kind == m::Value::Kind::Block && v->tag != Path::Pident &&
           !v->fields.empty()) {
      id = v->fields[0];
      v = &arena_[id];
    }
    return v->kind == m::Value::Kind::Block && v->tag == Path::Pident
               ? static_cast<int>(id) : -1;
  }
  PathPtr path(std::size_t id) {
    const m::Value& v = arena_[id];
    PathPtr p = path_alloc();
    p->kind = static_cast<Path::Kind>(v.tag);
    // Provenance: one id per path BLOCK of this file (memoised by arena
    // index, so two citations Marshal shared stay one object for the writer).
    if (cmi_id_ && id < path_prov_.size()) {
      int& pv = path_prov_[id];
      if (!pv) pv = prov_new(cmi_id_, head_blk(id), v.tag != Path::Pident);
      p->prov = pv;
    }
    switch (v.tag) {
      case Path::Pident:
        p->id = ident(v.fields.at(0));
        break;
      case Path::Pdot:
        p->a = path(v.fields.at(0));
        p->s = arena_[v.fields.at(1)].str();
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
    return arena_[v.fields.at(0)].str();                       // Some s
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
        // commutable: `Cok` is the int 0, `Cvar {..}` a one-field block.
        t.commu_var = d.fields.size() > 3 &&
                      arena_[d.fields[3]].kind == m::Value::Kind::Block;
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
          case 6: {  // Tvariant of row_desc
            t.kind = TypeExpr::Tvariant;
            // row_desc = { row_fields:(label*row_field) list; row_more;
            //              row_closed; row_fixed; row_name }.
            if (d.fields.empty()) break;
            const m::Value& rd = arena_[d.fields[0]];
            if (rd.fields.empty()) break;
            for (std::size_t cur = rd.fields[0];
                 arena_[cur].kind == m::Value::Kind::Block && !arena_[cur].fields.empty();
                 cur = arena_[cur].fields[1]) {       // cons cell: (head, tail)
              const m::Value& pair = arena_[arena_[cur].fields[0]];  // (label, row_field)
              if (pair.fields.size() < 2) continue;
              const m::Value& lbl = arena_[pair.fields[0]];
              if (lbl.kind != m::Value::Kind::String) continue;
              // row_field: RFpresent(te option) block 0 / RFeither{no_arg;
              // arg_type:te list; matched; ext} block 1 / RFabsent (Int).
              const m::Value& rf = arena_[pair.fields[1]];
              if (rf.kind == m::Value::Kind::Int) continue;  // RFabsent: dropped tag
              t.pv_tags.push_back(lbl.str());
              if (rf.tag == 0) {  // RFpresent
                t.pv_present.push_back(1);
                const m::Value& oa = arena_[rf.fields.at(0)];
                t.pv_args.push_back(oa.kind == m::Value::Kind::Int
                                        ? nullptr : type(oa.fields.at(0)));
              } else {            // RFeither
                t.pv_present.push_back(0);
                std::vector<TypePtr> ats = type_list(rf.fields.at(1));
                t.pv_args.push_back(ats.empty() ? nullptr : ats[0]);
              }
            }
            if (rd.fields.size() >= 3) {
              TypePtr more = type(rd.fields[1]);
              while (more && (more->kind == TypeExpr::Tlink ||
                              more->kind == TypeExpr::Tsubst))
                more = more->link;
              t.row_more_nil = more && more->kind == TypeExpr::Tnil;
              const m::Value& rc = arena_[rd.fields[2]];
              t.row_closed = rc.kind == m::Value::Kind::Int && rc.i != 0;
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
    t.label = arena_[v.fields.at(0)].str();
  }

  const m::Arena& arena_;
  std::vector<TypePtr> memo_;  // dense by arena id; null = not yet decoded
  std::vector<int> path_prov_;  // dense by arena id; 0 = no prov yet
  int cmi_id_ = 0;              // cmi_id_of(this file); 0 = no provenance
};

}  // namespace

// --- path provenance registry (cmi.hpp) ---------------------------------------
namespace {
std::vector<ProvInfo>& prov_table() {
  static std::vector<ProvInfo> t(1);  // index 0 = the predef global / unknown
  return t;
}
}  // namespace
int prov_new(int cmi, int head_blk, bool pdot) {
  auto& t = prov_table();
  t.push_back(ProvInfo{cmi, head_blk, pdot, {}, 0});
  return static_cast<int>(t.size()) - 1;
}
int prov_new_open(const std::string& open_pfx) {
  auto& t = prov_table();
  t.push_back(ProvInfo{0, -1, false, open_pfx, 0});
  return static_cast<int>(t.size()) - 1;
}
int prov_new_dots(int src_dots) {
  auto& t = prov_table();
  t.push_back(ProvInfo{0, -1, false, {}, src_dots});
  return static_cast<int>(t.size()) - 1;
}
const ProvInfo& prov_info(int prov) {
  auto& t = prov_table();
  return prov > 0 && static_cast<std::size_t>(prov) < t.size() ? t[prov] : t[0];
}
int cmi_id_of(const std::string& filepath) {
  static std::unordered_map<std::string, int> ids;
  auto& v = ids[filepath];
  if (!v) v = static_cast<int>(ids.size());
  return v;
}
bool prov_off() {
  static const bool off = cppcaml::dbg_env("NOPROV") != nullptr;
  return off;
}
bool node_id_off() {
  static const bool off = cppcaml::dbg_env("NONODEID") != nullptr;
  return off;
}

namespace {
// Read a .cmi file, locate its Marshal header, and decode the byte stream into
// `arena`; returns the id of the header value (the (modname, signature) tuple).
// Shared by both load paths -- the arena build is identical; only which parts of
// the signature the Decoder then materialises differs.
// Advance `off` to the next Marshal magic byte (0x84 0x95 0xA6 0xB{D,E,F}) at or
// after its current value; returns false if none is found before end.
bool find_marshal_magic(const std::vector<std::uint8_t>& bytes, std::size_t& off) {
  for (; off + 4 <= bytes.size(); ++off)
    if (bytes[off] == 0x84 && bytes[off + 1] == 0x95 && bytes[off + 2] == 0xA6 &&
        (bytes[off + 3] == 0xBE || bytes[off + 3] == 0xBF || bytes[off + 3] == 0xBD))
      return true;
  return false;
}

std::size_t read_cmi_arena(const std::string& filepath, m::Arena& arena,
                           std::vector<std::string>* imports = nullptr) {
  std::ifstream in(filepath, std::ios::binary);
  if (!in) throw m::Error("cannot open " + filepath);
  std::vector<std::uint8_t> bytes = slurp_bytes(in);

  // Skip the cmi magic string and decode the header value (name, signature).
  std::size_t off = 0;
  if (!find_marshal_magic(bytes, off)) throw m::Error("no Marshal magic in " + filepath);

  // The decoded node count is roughly proportional to the file size; reserve up
  // front so the arena (a vector of ~140-byte Values) does not repeatedly
  // reallocate and move every node as it grows during decode.  Node count runs
  // ~0.4-0.55x the file size across our .cmi corpus, so reserve 2/3 of it:
  // enough to avoid the grow-and-move-every-node realloc for every observed
  // file, without the 2x transient memory of reserving in full.  The arena is
  // freed when load() returns, so an over-reserve is only transient.
  arena.reserve(bytes.size() * 2 / 3);
  std::size_t header = m::read_value(bytes.data(), bytes.size(), off, arena);

  // The cmi_crcs table ("Interfaces imported") is a SECOND, independently
  // marshalled value right after the header (see file_formats/cmi_format.ml:
  // output_value oc (crcs : (modname * digest option) list)).  Decode just its
  // module names into the same arena when the caller wants the import set.
  bool have_crcs = false;
  std::size_t crcs = 0;
  if (imports && find_marshal_magic(bytes, off)) {
    try {
      crcs = m::read_value(bytes.data(), bytes.size(), off, arena);
      have_crcs = true;
    } catch (...) {}
  }

  // All marshal reads are done: seal every node's field span into the contiguous
  // pool so it is valid for the reads below and for the Decoder that consumes
  // this arena after we return.
  arena.finalize();

  if (have_crcs) {
    for (std::size_t cur = crcs; arena[cur].kind == m::Value::Kind::Block &&
                                 arena[cur].fields.size() == 2;) {
      const m::Value& pair = arena[arena[cur].fields[0]];  // (modname, digest opt)
      if (!pair.fields.empty()) imports->push_back(arena[pair.fields[0]].str());
      cur = arena[cur].fields[1];
    }
  }
  return header;
}
}  // namespace

namespace {
// The full-decode cache, at file scope so loaded_paths() can enumerate which
// modules this compile has actually referenced (their .cmi got loaded).
// Holds pointers: a decoded CmiFile either lives on the normal heap (never
// freed -- the cache is never erased and the compiler fast-exits) or inside a
// sealed read-only region (the mmap cmi cache).
std::unordered_map<std::string, const CmiFile*> g_load_cache;

#ifdef CPPCAML_CMI_REGIONS
// One region per cmi: a private VA reservation whose pages hold every byte
// reachable from the decoded CmiFile.  Real graphs are 0.05-3MB; 32MB gives
// 10x headroom over the largest observed while keeping the arena's metadata
// (bitmaps sized by slice count) small -- a graph that somehow outgrows it
// just falls back to a normal decode.  Pages commit on touch (MAP_NORESERVE).
constexpr std::size_t kRegionSize = 32ull << 20;
// Every cmi decodes AT A FIXED VIRTUAL ADDRESS derived from its absolute
// path, so a dumped region maps back later with all internal pointers valid
// as-is (the GCC-PCH trick): zero fixup, zero per-node work.  2^18 slots of
// one region each span 8TB starting at 0x4800'0000'0000 -- clear of the PIE
// executable range (~0x55..) and the top-down mmap area (~0x7f..) on x86-64
// Linux.  ~300 cmis into 2^18 slots makes a same-process collision unlikely;
// MAP_FIXED_NOREPLACE turns any collision or overlap into a clean fallback
// (normal decode, no cache) rather than a corrupted mapping.  32MB regions
// are naturally 64KB-aligned as mi_manage_os_memory_ex requires.
constexpr std::uintptr_t kSlotBase = 0x4800'0000'0000ull;
constexpr std::size_t kSlotCount = 1ull << 18;

std::uint64_t fnv1a64(const char* s) {
  std::uint64_t h = 1469598103934665603ull;
  for (; *s; ++s) h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull;
  return h;
}
void* slot_of(const char* abspath) {
  return reinterpret_cast<void*>(kSlotBase +
                                 (fnv1a64(abspath) % kSlotCount) * kRegionSize);
}

// The producing compiler's own GNU build-id (linked in via -Wl,--build-id).
// Blobs are stamped with it: ANY rebuild of the compiler -- decoder change,
// struct layout, libstdc++ headers -- yields a different id and silently
// invalidates every existing blob.  The blob format IS the process image, so
// this is the only versioning that can be trusted.  len 0 = no note found;
// the cache then disables itself entirely.
struct BuildId {
  unsigned char bytes[32] = {};
  unsigned len = 0;
};
const BuildId& own_build_id() {
  static const BuildId id = [] {
    BuildId out{};
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void* data) -> int {
          if (info->dlpi_name && info->dlpi_name[0]) return 0;  // main exe only
          auto* o = static_cast<BuildId*>(data);
          for (int i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_NOTE) continue;
            const char* p =
                reinterpret_cast<const char*>(info->dlpi_addr + ph.p_vaddr);
            const char* end = p + ph.p_memsz;
            while (p + sizeof(ElfW(Nhdr)) <= end) {
              auto* n = reinterpret_cast<const ElfW(Nhdr)*>(p);
              const char* name = p + sizeof(ElfW(Nhdr));
              const char* desc = name + ((n->n_namesz + 3) & ~3u);
              if (desc + n->n_descsz > end) break;
              if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 &&
                  std::memcmp(name, "GNU", 4) == 0) {
                o->len = n->n_descsz > sizeof(o->bytes) ? sizeof(o->bytes)
                                                        : n->n_descsz;
                std::memcpy(o->bytes, desc, o->len);
                return 1;
              }
              p = desc + ((n->n_descsz + 3) & ~3u);
            }
          }
          return 1;  // main executable processed; stop iterating
        },
        &out);
    return out;
  }();
  return id;
}

// Blob layout: one 4KB header page, then the region's bytes [base, base+used)
// at file offset 4096 (page-aligned, so the data maps directly).  Untouched
// pages inside `used` are file holes (written sparsely from the mincore map),
// so the on-disk footprint is the touched pages only.
struct CmimapHeader {
  char magic[8];                   // "CPPCMIM1"
  std::uint32_t version;           // bump on any layout change here
  std::uint32_t build_id_len;
  unsigned char build_id[32];
  std::uint64_t cmi_size;          // source .cmi stat: size
  std::uint64_t cmi_mtime_ns;      //   and mtime (ns)
  std::uint64_t region_base;       // the fixed slot this region decoded at
  std::uint64_t used;              // region bytes in this file
  std::uint64_t entry;             // the CmiFile* inside the region
  std::uint32_t path_len;
  char path[3968];                 // NUL-terminated absolute cmi path
};
static_assert(sizeof(CmimapHeader) <= 4096);
constexpr char kCmimapMagic[8] = {'C', 'P', 'P', 'C', 'M', 'I', 'M', '1'};
constexpr std::uint32_t kCmimapVersion = 1;

// CPPCAML_CMIMAP=<blob dir> opts the cache in; off by default.  Disabled
// (null) when the dir cannot be created or the compiler has no build-id.
const char* cmimap_dir() {
  static const char* d = []() -> const char* {
    const char* dir = std::getenv("CPPCAML_CMIMAP");
    if (!dir || !*dir) return nullptr;
    if (own_build_id().len == 0) return nullptr;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return ec ? nullptr : dir;
  }();
  return d;
}
bool cmimap_dbg() {
  static const bool b = std::getenv("CPPCAML_CMIMAP_DBG") != nullptr;
  return b;
}

std::string blob_path(const char* dir, const char* abspath) {
  const char* slash = std::strrchr(abspath, '/');
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx",
                static_cast<unsigned long long>(fnv1a64(abspath)));
  return std::string(dir) + "/" + (slash ? slash + 1 : abspath) + "-" + hex +
         ".cmimap";
}

// (size, mtime_ns) of the source .cmi -- the staleness key.
bool cmi_stat(const char* path, std::uint64_t& size, std::uint64_t& mtime_ns) {
  struct stat st;
  if (::stat(path, &st) != 0) return false;
  size = static_cast<std::uint64_t>(st.st_size);
  mtime_ns = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1000000000ull +
             static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
  return true;
}

// Write the sealed region [base, base+used) to <dir>/<blob>, sparsely (only
// resident pages; untouched ones stay holes), atomically (tmp + rename), and
// durably (fsync before rename -- a torn blob after a crash must not be
// mistakable for a complete one; there is no load-time checksum, checksums
// would defeat the zero-copy load).  Failure is silent: worst case the cmi
// just stays uncached.
void dump_blob(const char* dir, const char* abspath, std::uint64_t cmi_size,
               std::uint64_t cmi_mtime_ns, void* base, std::size_t used,
               const CmiFile* entry) {
  if (std::strlen(abspath) >= sizeof(CmimapHeader::path)) return;
  std::string final_path = blob_path(dir, abspath);
  std::string tmp = final_path + ".tmp." + std::to_string(::getpid());
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) return;
  bool ok = ::ftruncate(fd, 4096 + static_cast<off_t>(used)) == 0;

  CmimapHeader h{};
  std::memcpy(h.magic, kCmimapMagic, 8);
  h.version = kCmimapVersion;
  const BuildId& bid = own_build_id();
  h.build_id_len = bid.len;
  std::memcpy(h.build_id, bid.bytes, sizeof h.build_id);
  h.cmi_size = cmi_size;
  h.cmi_mtime_ns = cmi_mtime_ns;
  h.region_base = reinterpret_cast<std::uint64_t>(base);
  h.used = used;
  h.entry = reinterpret_cast<std::uint64_t>(entry);
  h.path_len = static_cast<std::uint32_t>(std::strlen(abspath));
  std::memcpy(h.path, abspath, h.path_len + 1);
  ok = ok && ::pwrite(fd, &h, sizeof h, 0) == static_cast<ssize_t>(sizeof h);

  // Sparse data: write runs of resident pages, seek over the rest.
  const std::size_t npages = (used + 4095) / 4096;
  std::vector<unsigned char> res(npages);
  ok = ok && ::mincore(base, used, res.data()) == 0;
  for (std::size_t i = 0; ok && i < npages;) {
    if (!(res[i] & 1)) { ++i; continue; }
    std::size_t j = i;
    while (j < npages && (res[j] & 1)) ++j;
    std::size_t off = i * 4096, len = std::min(j * 4096, used) - off;
    ok = ::pwrite(fd, static_cast<char*>(base) + off, len,
                  4096 + static_cast<off_t>(off)) == static_cast<ssize_t>(len);
    i = j;
  }
  ok = ok && ::fsync(fd) == 0;
  ::close(fd);
  if (ok) ok = ::rename(tmp.c_str(), final_path.c_str()) == 0;
  if (!ok) ::unlink(tmp.c_str());
  if (cmimap_dbg())
    std::fprintf(stderr, "[cmimap] dump %s: %s\n", final_path.c_str(),
                 ok ? "ok" : "FAILED");
}
#endif
}  // namespace

#ifdef CPPCAML_CMI_REGIONS
const CmiFile* CmiFile::decode_in_region(const std::string& filepath,
                                         const m::Arena& arena,
                                         std::size_t header,
                                         const std::vector<std::string>& imports) {
  // The region must live at the path's fixed slot, or the dump would be
  // useless (pointers are absolute).  MAP_FIXED_NOREPLACE: a slot collision
  // or any pre-existing overlap fails cleanly and we just decode normally.
  const char* dir = cmimap_dir();
  char abspath[PATH_MAX];
  if (!dir || !::realpath(filepath.c_str(), abspath)) return nullptr;
  std::uint64_t cmi_size = 0, cmi_mtime_ns = 0;
  if (!cmi_stat(abspath, cmi_size, cmi_mtime_ns)) return nullptr;
  void* slot = slot_of(abspath);
  void* base = mmap(slot, kRegionSize, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                        MAP_FIXED_NOREPLACE,
                    -1, 0);
  if (base == MAP_FAILED) return nullptr;

  // Hand the region to mimalloc as an exclusive arena and decode under a heap
  // bound to it.  Exclusive means allocations from that heap come from this
  // region or fail loudly (no silent OS fallback): containment is guaranteed,
  // not hoped for.  bad_alloc on a graph outgrowing the region falls back to
  // a normal decode.
  mi_arena_id_t aid = nullptr;
  if (!mi_manage_os_memory_ex(base, kRegionSize, /*is_committed=*/true,
                              /*is_pinned=*/true, /*is_zero=*/true,
                              /*numa_node=*/-1, /*exclusive=*/true, &aid)) {
    munmap(base, kRegionSize);
    return nullptr;
  }
  auto unload_arena = [&] {
    void* rb = nullptr;
    std::size_t accessed = 0, full = 0;
    mi_arena_unload(aid, &rb, &accessed, &full);
    return accessed;
  };
  mi_heap_t* heap = mi_heap_new_in_arena(aid);
  if (!heap) {
    unload_arena();
    munmap(base, kRegionSize);
    return nullptr;
  }

  // Decode with the region heap as the default allocation target.  The
  // transient marshal arena was already parsed on the normal heap (it is
  // discarded after decode -- caching it was the proven session-14 wash),
  // and the Decoder's memo is a normal-heap transient (see Decoder);
  // everything allocated from here to the restore -- the CmiFile object, its
  // strings/vectors, every graph node slab -- lands in the region.
  CmiFile* cmi = nullptr;
  // No provenance in a region decode: the dump is reused by later processes
  // whose prov numbering differs, so a baked-in id would alias a stranger's.
  Decoder dec(arena);
  mi_heap_t* prev = mi_heap_set_default(heap);
  g_graph_arena.fresh();
  try {
    cmi = new CmiFile();
    cmi->module_name_ = std::string(arena[arena[header].fields.at(0)].str());
    cmi->imports_ = imports;  // deep-copies into the region
    cmi->sig_ = dec.signature(arena[header].fields.at(1));
  } catch (...) {
    g_graph_arena.fresh();
    mi_heap_set_default(prev);
    mi_heap_unload(heap);
    unload_arena();
    munmap(base, kRegionSize);
    return nullptr;  // fall back to a normal decode
  }
  g_graph_arena.fresh();
  mi_heap_set_default(prev);

  // Seal: detach the arena from mimalloc (nothing may allocate or free here
  // ever again -- consumers only read), then enforce exactly that.  Any code
  // path that mutates a decoded graph now faults instead of silently
  // corrupting the dump: PROT_READ is the containment probe.
  mi_heap_unload(heap);
  std::size_t accessed = unload_arena();
  mprotect(base, kRegionSize, PROT_READ);
  if (cmimap_dbg()) {
    // The dump-size truth is the pages actually touched (resident), not the
    // slice-granular `accessed`: mimalloc's 64KB slices inflate the latter.
    std::vector<unsigned char> res((accessed + 4095) / 4096);
    std::size_t touched = 0;
    if (!res.empty() && mincore(base, accessed, res.data()) == 0)
      for (unsigned char r : res) touched += (r & 1) ? 4096 : 0;
    std::fprintf(stderr, "[cmimap] %s: region %p accessed %zuKB touched %zuKB\n",
                 filepath.c_str(), base, accessed >> 10, touched >> 10);
  }
  dump_blob(dir, abspath, cmi_size, cmi_mtime_ns, base, accessed, cmi);
  return cmi;
}

const CmiFile* CmiFile::load_from_blob(const std::string& filepath) {
  const char* dir = cmimap_dir();
  char abspath[PATH_MAX];
  if (!dir || !::realpath(filepath.c_str(), abspath)) return nullptr;
  std::uint64_t cmi_size = 0, cmi_mtime_ns = 0;
  if (!cmi_stat(abspath, cmi_size, cmi_mtime_ns)) return nullptr;
  int fd = ::open(blob_path(dir, abspath).c_str(), O_RDONLY);
  if (fd < 0) return nullptr;

  // Validate the header: right format, right COMPILER BUILD, right source
  // .cmi (size+mtime), right path (guards a blob-name hash collision), and
  // an entry pointer inside the region.  Any mismatch -> decode normally
  // (and the cold path will rename a fresh blob over this one).
  CmimapHeader h;
  bool ok = ::pread(fd, &h, sizeof h, 0) == static_cast<ssize_t>(sizeof h) &&
            std::memcmp(h.magic, kCmimapMagic, 8) == 0 &&
            h.version == kCmimapVersion;
  const BuildId& bid = own_build_id();
  ok = ok && h.build_id_len == bid.len &&
       std::memcmp(h.build_id, bid.bytes, sizeof h.build_id) == 0;
  ok = ok && h.cmi_size == cmi_size && h.cmi_mtime_ns == cmi_mtime_ns;
  ok = ok && h.path_len < sizeof h.path && h.path[h.path_len] == '\0' &&
       std::strcmp(h.path, abspath) == 0;
  void* slot = slot_of(abspath);
  ok = ok && h.region_base == reinterpret_cast<std::uint64_t>(slot);
  ok = ok && h.used > 0 && h.used <= kRegionSize &&
       h.entry >= h.region_base + sizeof(void*) &&
       h.entry + sizeof(CmiFile) <= h.region_base + h.used;
  if (!ok) {
    ::close(fd);
    return nullptr;
  }

  // Map the region bytes back at their recorded address, read-only, private.
  // File offset 4096 is page-aligned; holes read back as the zero pages they
  // were.  The pointers inside are valid the instant the map exists.
  void* m = mmap(slot, h.used, PROT_READ,
                 MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 4096);
  ::close(fd);  // the mapping keeps the file alive
  if (m == MAP_FAILED) return nullptr;
  if (cmimap_dbg())
    std::fprintf(stderr, "[cmimap] warm %s: %zuKB at %p\n", filepath.c_str(),
                 static_cast<std::size_t>(h.used) >> 10, m);
  return reinterpret_cast<const CmiFile*>(h.entry);
}
#endif  // CPPCAML_CMI_REGIONS

const CmiFile& CmiFile::load(const std::string& filepath) {
  // A .cmi is immutable for the lifetime of a compile, but several passes (the
  // inferencer, register_stdlib_ctors, pervasive resolution) each re-decode the
  // same file -- stdlib.cmi alone is decoded 3x.  Memoise by path: the Marshal
  // decode (the costly part, ~2 ms for stdlib.cmi) then happens once.  Return a
  // reference to the (static, never-erased) cache entry so callers that bind
  // `const auto&` share the decoded signature instead of deep-copying it (the
  // whole SigValue/ConstructorDecl/LabelDecl graph) on every access.
  auto& cache = g_load_cache;
  if (auto it = cache.find(filepath); it != cache.end()) return *it->second;
#ifdef CPPCAML_CMI_REGIONS
  // Warm path of the mmap cmi cache: map a previously dumped, pre-decoded
  // region back at its fixed address -- no read, no Marshal parse, no graph
  // build.  Falls through to a normal decode on any mismatch.
  if (cmimap_dir())
    if (const CmiFile* r = load_from_blob(filepath))
      return *cache.emplace(filepath, r).first->second;
#endif
  m::Arena arena;
  auto cmi = std::make_unique<CmiFile>();
  // Fill imports() too (the crc table is a cheap tail read): the labelset index
  // reuses full-loaded cmis and needs their transitive-import set for scoping.
  std::size_t header = read_cmi_arena(filepath, arena, &cmi->imports_);
  const m::Value& tuple = arena[header];  // (modname, signature)

#ifdef CPPCAML_CMI_REGIONS
  if (cmimap_dir())
    if (const CmiFile* r =
            decode_in_region(filepath, arena, header, cmi->imports_))
      return *cache.emplace(filepath, r).first->second;
#endif
  Decoder dec(arena, cmi_id_of(filepath));
  cmi->module_name_ = arena[tuple.fields.at(0)].str();
  cmi->sig_ = dec.signature(tuple.fields.at(1));
  return *cache.emplace(filepath, cmi.release()).first->second;
}

std::vector<std::string> CmiFile::loaded_paths() {
  std::vector<std::string> out;
  out.reserve(g_load_cache.size());
  for (auto& [p, _] : g_load_cache) out.push_back(p);
  return out;
}

const CmiFile& CmiFile::load_types_only(const std::string& filepath) {
  // A SEPARATE cache from load(): this stores a partial signature (only
  // sig().types), so it must never satisfy a caller that expects a full decode.
  // The arena is still fully read (the Marshal stream is sequential), but the
  // Decoder skips the value type-graphs and submodule recursion.
  static std::unordered_map<std::string, CmiFile> cache;
  if (auto it = cache.find(filepath); it != cache.end()) return it->second;
  m::Arena arena;
  CmiFile cmi;
  std::size_t header = read_cmi_arena(filepath, arena, &cmi.imports_);
  const m::Value& tuple = arena[header];  // (modname, signature)

  Decoder dec(arena, cmi_id_of(filepath));
  cmi.module_name_ = arena[tuple.fields.at(0)].str();
  cmi.sig_ = dec.signature_types_only(tuple.fields.at(1));
  return cache.emplace(filepath, std::move(cmi)).first->second;
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
// S588: a label's, a constructor's, a module's, a module type's and an
// extension constructor's attributes are written too.  NOATTRPOS=1 writes
// their empty lists again.
bool attrpos_off() {
  static const bool off = cppcaml::dbg_env("NOATTRPOS") != nullptr ||
                          cppcaml::dbg_env("NOSHARE588") != nullptr;
  return off;
}

// S589: an attribute whose payload is one simple expression is written with
// it.  NOPAYLOAD=1 drops the whole attribute list again, as before.
bool payload_off() {
  static const bool off = cppcaml::dbg_env("NOPAYLOAD") != nullptr ||
                          cppcaml::dbg_env("NOSHARE589") != nullptr;
  return off;
}

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
TyPtr ty_poly(TyPtr body, std::vector<int> poly_ids) {
  auto t = std::make_shared<Ty>(); t->k = Ty::Poly;
  t->args = {std::move(body)}; t->poly_ids = std::move(poly_ids); return t; }
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
      {"floatarray", 19}, {"iarray", 20}, {"atomic_loc", 21}, {"todo_info", 22}};
  auto it = s.find(n);
  return it == s.end() ? 0 : it->second;
}

// --- physical sharing: one Value per object ocamlc's heap holds once --------
// The marshaller emits a two-byte CODE_SHARED back-reference for a value it has
// already written, and several of the values a signature cites over and over
// are ONE object up there: every Lexing.position in a file points at the same
// pos_fname string, `Location.none` is a single record built at module init
// (warnings.ml:716), each `Predef.path_*` is a single Path.t, and
// rename_bound_idents stores one `Pident id'` per bound ident which
// Subst.type_path then hands to every citation (subst.ml:594).  We built a
// fresh Value per citation, so patmatch.cmi carried 977 copies of
// "patmatch.ml" and 511 of "Patmatch" -- 1652 back-references in ocamlc's .cmi
// against our 24.  The tables are per-cmi (cleared by the writers).
// NOCMISHARE=1 reverts to a copy per citation.
struct CmiShare {
  std::map<std::string, o::ValPtr> strs;            // pos_fname / unit name
  std::map<std::string, o::ValPtr> poss;   // Lexing.position, by content
  std::map<std::string, o::ValPtr> ids;    // "tag:stamp:name@origin" -> Ident.t
  std::map<const o::Value*, o::ValPtr> pidents;     // Ident.t -> Path.Pident
  // S555, path PROVENANCE (cmi.hpp prov_new): a predef Pident per path
  // object, a global unit's head `Pident (Global u)` per origin (the initial
  // open's root, a unit's own prefix root, a .cmi's unmarshalled block), and
  // a Pdot component string per path object.  NOPROV=1 reverts.
  // S571: keyed "<prov>:<stamp>:<name>", not by prov alone.  A prov is one
  // written source node, and `T with type t := float` SUBSTITUTES the path
  // that node's lookup built (Subst.type_path hands on the substituting
  // object), so one node can reach the writer under several names -- six, in
  // translprim/module_coercion.  Sharing on prov alone gave all six
  // instantiations the FIRST name's block (`t array` became `int array`
  // everywhere).  Two citations are the same path object only if they also
  // name the same type.
  std::map<std::string, o::ValPtr> ppaths;          // prov:stamp:name -> Pident
  std::map<std::string, o::ValPtr> heads;           // head key -> Pident(Global)
  std::map<std::string, o::ValPtr> pstrs;           // "prov:i:s" -> string
  // S566: "<module stamp>:<member>" -> that member's own ident stamp, so a
  // `N.t` component can cite the string inside `type t`'s Ident block.
  std::map<std::string, int> mstamps;
  // S574: A RE-EXPORTED DECLARATION IS ONE DECLARATION.  `include A`, a
  // functor application's result and a strengthened alias all build their
  // signature through `Subst`, which rebuilds the type but carries the
  // declaration's name string (`rename_bound_idents` is `Ident.create_local
  // (Ident.name id)` -- a fresh stamp over the SAME string), its location
  // (`Subst.loc` is the identity while locations are kept) and its uid
  // (copied verbatim), so ocamlc's heap holds ONE name string, ONE Location.t
  // and ONE Uid block per declaration however many signatures re-export it.
  // Keyed on the three TOGETHER: same uid, same name, same span = the same
  // declaration.  A key that agreed on the uid alone would also fuse two
  // declarations our own numbering happened to give one id (w60's two
  // functor parameters both declare `module Id : Comparable`), which is a
  // separate bug and not this law's to paper over.
  // A CONSTRUCTOR / LABEL goes further: `rename_bound_idents` renames only a
  // signature's own items, so `Subst.constructor_declaration` carries `cd_id`
  // (and `ld_id`) THEMSELVES -- the whole Ident block, stamp and all.
  std::map<std::string, o::ValPtr> decl_strs, decl_locs, decl_uids, decl_ids;
  o::ValPtr none;                                   // Location.none
  o::ValPtr tvar_none, tunivar_none;                // the two shared descs
  o::ValPtr prim_noname;                            // the `""` native name
  // S578: `Unboxed_integer Pint32/Pint64/Pnativeint` is a STATIC constant in
  // Typedecl.native_repr_of_type, so every external's argument or result
  // cites the ONE block per kind (keyed by the writer's repr code 3/4/5).
  std::map<int, o::ValPtr> reprs;
  void clear() {
    strs.clear(); poss.clear(); ids.clear(); pidents.clear(); none = nullptr;
    ppaths.clear(); heads.clear(); pstrs.clear(); mstamps.clear();
    decl_strs.clear(); decl_locs.clear(); decl_uids.clear(); decl_ids.clear();
    tvar_none = tunivar_none = nullptr; prim_noname = nullptr;
    reprs.clear();
  }
};
CmiShare g_share;
// S554: the class item's shape (locations, the nominal self type, the hash
// type's manifest, the meths-map spine copies, the shared name / loc / uid).
// NOCLSITEM=1 reverts to the S553 emitter.
bool clsitem_off_() {
  static const bool off = cppcaml::dbg_env("NOCLSITEM") != nullptr;
  return off;
}
// S561: a type declaration's type_unboxed_default and a record's
// Record_float representation.  NOUNBOXDEF=1 writes false / Record_regular
// for every declaration (the S560 emitter).
bool unboxdef_off() {
  static const bool off = cppcaml::dbg_env("NOUNBOXDEF") != nullptr;
  return off;
}
bool no_share() {
  static const bool off = cppcaml::dbg_env("NOCMISHARE") != nullptr;
  return off;
}
bool prov_off_() { return prov_off(); }
o::ValPtr shared_str(const std::string& s) {
  if (no_share()) return o::vstr(s);
  auto& v = g_share.strs[s];
  if (!v) v = o::vstr(s);
  return v;
}
// S574: the sharing key of a declaration (see CmiShare::decl_strs).  Only an
// `Item` uid names a declaration; Internal is "no uid at all" and
// Predef/CompUnit name no item.  NOUIDSHARE=1 (alias NOSHARE574=1) writes a
// fresh name / location / uid block per occurrence again.
bool uidshare_off() {
  static const bool off = cppcaml::dbg_env("NOUIDSHARE") != nullptr ||
                          cppcaml::dbg_env("NOSHARE574") != nullptr;
  return off;
}
std::string pos_key(const cmiw::WPos& p) {
  return p.fname + "/" + std::to_string(p.file_id) + ":" +
         std::to_string(p.lnum) + ":" + std::to_string(p.bol) + ":" +
         std::to_string(p.cnum);
}
std::string decl_key(const std::string& name, const cmiw::Loc& l,
                     const cmiw::Uid& u) {
  if (u.k != cmiw::Uid::Item || no_share() || uidshare_off()) return "";
  return u.unit + "/" + std::to_string(u.id) + (u.intf ? "/i/" : "/m/") +
         name + "@" + (l.ghost ? "g" : pos_key(l.start) + "-" + pos_key(l.end));
}
// NOFWDIDSTR=1 gives a type cited ahead of its own item a fresh name string.
bool fwdidstr_off() {
  static const bool off = cppcaml::dbg_env("NOFWDIDSTR") != nullptr ||
                          cppcaml::dbg_env("NOSHARE584") != nullptr;
  return off;
}
// A declaration's name string, shared with every signature that re-exports
// that same declaration (S574).
o::ValPtr decl_name_str(const std::string& n, const std::string& key) {
  if (key.empty()) return o::vstr(n);
  auto& v = g_share.decl_strs[key];
  if (!v) v = o::vstr(n);
  return v;
}
// A constructor's / label's whole Ident block, shared with every signature
// that re-exports its declaration (S574): `Subst` never renames one.
o::ValPtr decl_ident(const std::string& n, int stamp, const std::string& key) {
  if (key.empty()) return o::vblock(0, {o::vstr(n), o::vint(stamp)});
  auto& v = g_share.decl_ids[key];
  if (!v) v = o::vblock(0, {o::vstr(n), o::vint(stamp)});
  return v;
}
// S565: an external that names no C stub gets `Primitive.description`'s
// `prim_native_name = ""` -- primitive.ml's own literal (`init_native_name`
// :150), one string for the whole process -- so a unit's externals all cite
// THAT object.  A native name that was written is the parsetree's own string,
// one per declaration.  NOPRIMNONAME=1 writes a fresh empty string again.
bool primnoname_off() {
  static const bool off = cppcaml::dbg_env("NOPRIMNONAME") != nullptr ||
                          cppcaml::dbg_env("NOSHARE565") != nullptr;
  return off;
}
o::ValPtr prim_native_str(const std::string& s) {
  if (!s.empty() || no_share() || primnoname_off()) return o::vstr(s);
  if (!g_share.prim_noname) g_share.prim_noname = o::vstr(s);
  return g_share.prim_noname;
}
// Ident.t: Local{name;stamp} is tag 0, Predef{name;stamp} tag 3.  `origin`
// (predef only) is the .cmi whose unmarshalled Ident block this is -- another
// unit's `int` is NOT Predef.ident_int's block -- 0 for Predef's own.
std::string ident_key(int tag, const std::string& name, int stamp,
                      int origin = 0) {
  return std::to_string(tag) + ":" + std::to_string(stamp) + ":" + name + "@" +
         std::to_string(origin);
}
o::ValPtr ident_val(int tag, const std::string& name, int stamp,
                    int origin = 0, o::ValPtr name_val = nullptr) {
  if (!name_val) name_val = o::vstr(name);
  if (no_share()) return o::vblock(tag, {name_val, o::vint(stamp)});
  auto& v = g_share.ids[ident_key(tag, name, stamp, origin)];
  if (!v) v = o::vblock(tag, {name_val, o::vint(stamp)});
  return v;
}
o::ValPtr pident(const o::ValPtr& id) {  // Path.Pident
  if (no_share()) return o::vblock(0, {id});
  auto& v = g_share.pidents[id.get()];
  if (!v) v = o::vblock(0, {id});
  return v;
}
o::ValPtr pident_local(const std::string& name, int stamp) {
  return pident(ident_val(0, name, stamp));
}
// A predef type's path: prov 0 is `Predef.path_<name>` (one Path.t per
// process -- a literal, `()`, a predef-typed primitive); prov k > 0 is one
// distinct `Pident` block (an annotation's lookup, or a .cmi's) over the
// origin's Ident.
o::ValPtr pident_predef(const std::string& name, int stamp, int prov = 0) {
  if (prov_off_()) prov = 0;
  const ProvInfo& pi = prov_info(prov);
  o::ValPtr id = ident_val(3, name, stamp, pi.cmi);
  if (prov == 0 || no_share()) return pident(id);
  auto& v = g_share.ppaths[std::to_string(prov) + ":" +
                           std::to_string(stamp) + ":" + name];
  if (!v) v = o::vblock(0, {id});
  return v;
}
// S568: AN EXCEPTION'S EXTENDED TYPE IS THE ONE `Predef.path_exn`.  A plain
// exception is `Text_exception` over `Predef.path_exn` (typedecl.ml's
// `transl_exception` -> `Predef.path_exn`, and env.ml's initial environment
// holds the same object), which is a MODULE-LEVEL binding in predef.ml, built
// once per process and never rebuilt -- unlike a written `exn` annotation,
// which is an `Env.lookup_type` and so gets its own `Pident` block over the
// shared Predef ident (S567).  So every exception a unit declares cites ONE
// path object, block + Ident + name string alike, and a .cmi with two
// exceptions back-references the second.  NOEXNPATH=1 writes a fresh block.
bool exnpath_off() {
  static const bool off = cppcaml::dbg_env("NOEXNPATH") != nullptr ||
                          cppcaml::dbg_env("NOSHARE568") != nullptr;
  return off;
}
o::ValPtr predef_exn_path() {
  if (exnpath_off())
    return o::vblock(0, {o::vblock(3, {o::vstr("exn"), o::vint(7)})});
  return pident_predef("exn", 7, 0);
}
// S569: A NAME THE ENVIRONMENT RESOLVED IN A LOADED UNIT IS THAT UNIT'S ONE
// ROOT AND THAT DECLARATION'S ONE NAME.  `Env.sign_of_cmi` (env.ml:937) builds
// ONE `path = Pident id` per .cmi it loads, and every component reached
// through that unit is `Pdot (root, Ident.name id)` -- `prefix_idents` over
// the loaded signature's own idents (S566) -- so `Arg.spec` and `Arg.key`,
// however many citations and whichever value dragged them in, hang off that
// one root and hand on the one name string each declaration's Ident carries.
// Subst rebuilds the Pdot BLOCKS per citation (S565) but nothing below them.
// The counter-cases keep their own objects: a head the SOURCE wrote
// (`find_name_module`, env.ml:872, allocates a `Pident` per lookup), a
// trailing component the source wrote (`lookup_dot_type`'s `s.txt`, S566),
// and a Pdot chain decoded verbatim out of another .cmi (that file's blocks).
// NOENVPATH=1 mints a fresh block and fresh strings for a citation with no
// provenance, and keys an environment-added component by provenance again.
// S575: A FUNCTION TYPE AN APPLICATION INVENTED KEEPS ITS COMMUTATION UNKNOWN.
// The writer's half of the law (the engine's is infer.cpp's commu_off): emit
// `Cok` for every arrow again.
bool commu_off() {
  static const bool off = cppcaml::dbg_env("NOCOMMU") != nullptr ||
                          cppcaml::dbg_env("NOSHARE575") != nullptr;
  return off;
}
// S576: A DECLARATION'S PARAMETERS CARRY THEIR SEPARABILITY
// (Typedecl_separability).  NOSEP=1 writes `Ind` for every parameter again.
bool sep_off() {
  static const bool off = cppcaml::dbg_env("NOSEP") != nullptr ||
                          cppcaml::dbg_env("NOSHARE576") != nullptr;
  return off;
}
// S577: A DECLARATION'S EMPTY-PAYLOAD ATTRIBUTES ARE WRITTEN.  NOATTR=1
// writes the empty lists (and the placeholder `[@@immediate]`) again.
bool attr_off() {
  static const bool off = cppcaml::dbg_env("NOATTR") != nullptr ||
                          cppcaml::dbg_env("NOSHARE577") != nullptr;
  return off;
}
// S578: an unboxed integer's native_repr is one shared block per kind.
// NOREPRSHARE=1 allocates one per use again.
bool reprshare_off() {
  static const bool off = cppcaml::dbg_env("NOREPRSHARE") != nullptr ||
                          cppcaml::dbg_env("NOSHARE578") != nullptr;
  return off;
}
// S580: an alias of a stdlib member cites it through the initial open.
// NOSTDALIAS=1 writes the bare unit global again.
bool stdalias_off() {
  static const bool off = cppcaml::dbg_env("NOSTDALIAS") != nullptr ||
                          cppcaml::dbg_env("NOSHARE580") != nullptr;
  return off;
}
// S587: an applied path's pervasive head (`Set.Make (X).t`) is the initial
// open's root too.  NOAPPOPEN=1 writes a fresh `Pident Stdlib` again.
bool appopen_off() {
  static const bool off = cppcaml::dbg_env("NOAPPOPEN") != nullptr ||
                          cppcaml::dbg_env("NOSHARE587") != nullptr;
  return off;
}
// The root a pervasive module head is looked up through: `IdTbl.find_name`
// resolves `Set` to `Pdot (root, "Set")` over the initial open's ONE root,
// whether the path is a type's, an application's or a module type's (S587).
o::ValPtr open_stdlib_root() {
  if (no_share() || appopen_off())
    return o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})});
  auto& v = g_share.heads["open:Stdlib"];
  if (!v) v = o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})});
  return v;
}
bool envpath_off() {
  static const bool off = cppcaml::dbg_env("NOENVPATH") != nullptr ||
                          cppcaml::dbg_env("NOSHARE569") != nullptr;
  return off;
}
// Did the ENVIRONMENT build this citation's path out of a loaded unit's own
// declarations?  A citation with no provenance did (the writer qualified a
// bare name itself), and so did one decoded from a .cmi as a `Pident` this
// unit then qualifies.  A name LOOKUP does not, even a bare one resolved
// through an `open`: `IdTbl.find_name` (env.ml:389-401) returns `Pdot (root,
// name)` over the string the LOOKUP passed, one per lookup site.
bool env_built(int prov) {
  if (envpath_off()) return false;
  if (prov == 0) return true;
  const ProvInfo& pi = prov_info(prov);
  return pi.cmi != 0 && !pi.pdot;
}
// The `Ident.t` inside the `Pident (Global unit)` a WRITTEN head resolves to:
// the one persistent ident that unit's entry in `env.modules` carries.  The
// lookup builds a fresh `Pident` BLOCK over it (env.ml:389-401, :872 --
// `Pident id` allocates, `id` does not), so `Stdlib.List.t` written twice is
// two blocks over one ident.  Every other head keeps its own.
o::ValPtr written_head_ident(const std::string& unit) {
  if (no_share() || envpath_off()) return o::vblock(2, {o::vstr(unit)});
  auto& v = g_share.heads["gid:" + unit];
  if (!v) v = o::vblock(2, {o::vstr(unit)});
  return v;
}
// The name string of `s`, the `i`th component of a path into `owner`'s own
// declarations: that declaration's ident name, one per unit member.
o::ValPtr unit_member_str(const std::string& owner, int i,
                          const std::string& s) {
  if (no_share()) return o::vstr(s);
  auto& v = g_share.pstrs["gmem:" + owner + ":" + std::to_string(i) + ":" + s];
  if (!v) v = o::vstr(s);
  return v;
}
// The head `Pident (Global unit)` of a Pdot chain, by provenance: prov 0 is
// that unit's own root (S569) unless the source WROTE the head; an
// annotation's head is the initial open's root when the name routed through
// it (`List.t`, `ref`), else fresh per lookup (`Stdlib.List.t`, `Foo.t` --
// find_name_module allocates); a .cmi
// Pident's head is that unit's prefix root (sign_of_cmi's one `Pident id`);
// a Pdot stored in a .cmi keeps that file's block.
o::ValPtr global_head(const std::string& unit, int prov, bool explicit_head) {
  if (prov_off_()) prov = 0;
  if (no_share() || (prov == 0 && (explicit_head || envpath_off())))
    return o::vblock(0, {written_head_ident(unit)});
  const ProvInfo& pi = prov_info(prov);
  std::string key;
  if (!pi.open_pfx.empty()) explicit_head = false;  // the open's root
  if (prov == 0)
    key = "unit:" + unit;  // S569: the root sign_of_cmi made for that unit
  else if (pi.cmi == 0)
    key = explicit_head ? "prov:" + std::to_string(prov) : "open:" + unit;
  else if (pi.pdot)
    key = "cmi:" + std::to_string(pi.cmi) + ":" + std::to_string(pi.head_blk);
  else
    key = "unit:" + unit;
  auto& v = g_share.heads[key];
  if (!v)
    v = o::vblock(0, {key.compare(0, 5, "prov:") == 0
                          ? written_head_ident(unit)
                          : o::vblock(2, {o::vstr(unit)})});
  return v;
}
// A Pdot component's string: one per path object (a copy of an annotation, or
// every citation of a .cmi block, cites the same string; Subst.type_path
// rebuilds the Pdot blocks fresh per citation but hands on `n`).  A component
// inside the prefix an `open` supplied is the open's own (its root path's
// string, one per open), shared by every name resolved through that open.
o::ValPtr comp_str(const std::string& s, int prov, int i) {
  if (prov_off_()) prov = 0;
  if (prov == 0 || no_share()) return o::vstr(s);
  const ProvInfo& pi = prov_info(prov);
  std::string key;
  if (!pi.open_pfx.empty()) {
    int k = 1;  // components in the open prefix
    for (char c : pi.open_pfx) if (c == '.') ++k;
    if (i < k) key = "open:" + pi.open_pfx + ":" + std::to_string(i) + ":" + s;
  }
  if (key.empty())
    key = std::to_string(prov) + ":" + std::to_string(i) + ":" + s;
  auto& v = g_share.pstrs[key];
  if (!v) v = o::vstr(s);
  return v;
}
// S566: A PATH'S COMPONENT STRING IS THE NAME OBJECT OF THE DECLARATION IT
// NAMES.  `prefix_idents` (env.ml:1728-1767) builds every path into a
// module's components as `Pdot(root, Ident.name id)` -- the signature item's
// OWN name string, the one that module's Ident block carries -- and
// `Mtype.strengthen` (mtype.ml:78) builds its manifests the same way.  So
// every citation of `N.t` in this unit hands on the string inside `type t`'s
// ident: Subst rebuilds the Pdot BLOCK per citation but passes `n` through
// untouched (subst.ml:90-132, S565).  NOMEMBSTR=1 writes a fresh string.
bool membstr_off() {
  static const bool off = cppcaml::dbg_env("NOMEMBSTR") != nullptr ||
                          cppcaml::dbg_env("NOSHARE566") != nullptr;
  return off;
}
// The ident stamp of `name` declared by the local module `owner`, 0 when this
// unit has not emitted that module's signature (nothing to cite yet).
int member_stamp(int owner, const std::string& name) {
  if (!owner || no_share() || membstr_off()) return 0;
  auto f = g_share.mstamps.find(std::to_string(owner) + ":" + name);
  return f == g_share.mstamps.end() ? 0 : f->second;
}
o::ValPtr member_str(int mstamp, const std::string& name) {
  return ident_val(0, name, mstamp)->fields[0];
}
// The Stdlib alias member on a `Stdlib.List.t`-style chain: a .cmi Pident's
// (List's own `t`, reached through Stdlib's components) is the alias ident's
// name string, one per alias; anything else is the path object's own.
o::ValPtr alias_str(const std::string& member, int prov) {
  if (prov_off_()) prov = 0;
  const ProvInfo& pi = prov_info(prov);
  const bool envq = prov == 0 ? !envpath_off() : (pi.cmi != 0 && !pi.pdot);
  if (no_share() || !envq) return comp_str(member, prov, 0);
  auto& v = g_share.pstrs["alias:" + member];
  if (!v) v = o::vstr(member);
  return v;
}
// subst.ml's `norm` (:157-162) exists for exactly this: it rewrites every
// anonymous `Tvar None` / `Tunivar None` desc to one shared value, so a saved
// signature holds ONE of each however many variables it has.
o::ValPtr var_none_desc(bool univar) {
  if (no_share()) return o::vblock(univar ? 7 : 0, {o::vint(0)});
  auto& v = univar ? g_share.tunivar_none : g_share.tvar_none;
  if (!v) v = o::vblock(univar ? 7 : 0, {o::vint(0)});
  return v;
}

// --- module resolution (for qualified `M.t` Tconstr paths + import CRCs) -----
std::string g_stdlib_dir = "stdlib";
std::vector<std::string> g_module_dirs;

// A source module head ("Buffer", "List", a local "A") -> its compilation-unit
// global ("Stdlib__Buffer", "A").  Mirrors lambda's global_of.
// Member ALIAS modules of stdlib.cmi (`module Bigarray = Stdlib__Bigarray`):
// loaded lazily like stdlib_toplevel_type.
static bool stdlib_alias_module(const std::string& n) {
  static std::set<std::string>* names = nullptr;
  if (!names) {
    names = new std::set<std::string>();
    try {
      for (const auto& m : CmiFile::load(g_stdlib_dir + "/stdlib.cmi").sig().modules)
        names->insert(m.name);
    } catch (const std::exception&) {}  // -nostdlib: set stays empty
  }
  return names->count(n) > 0;
}

std::string global_of(const std::string& mod) {
  // "Stdlib/2" is the checker's out-of-scope marker (the REAL Stdlib when the
  // file binds its own `module Stdlib`); the cmi stores the plain unit global.
  if (mod == "Stdlib/2") return "Stdlib";
  if (mod == "Stdlib" || mod.rfind("Stdlib__", 0) == 0) return mod;
  if (mod.rfind("Camlinternal", 0) == 0) return mod;
  if (std::filesystem::exists(g_stdlib_dir + "/stdlib__" + mod + ".cmi"))
    return "Stdlib__" + mod;
  // The cmi-file probe misses a stdlib member whose unit builds LATER in
  // the stdlib order (out_channel.mli cites Bigarray, built after it):
  // ocamlc resolves through Stdlib's ALIAS module regardless, so consult
  // stdlib.cmi's member list too -- the degraded bare `Bigarray` unit
  // global was unloadable and the bootstrapped compiler typed every
  // In_channel/Out_channel bigarray val abstract (bytelink/emitcode).
  if (stdlib_alias_module(mod)) return "Stdlib__" + mod;
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
  std::vector<std::uint8_t> bytes = slurp_bytes(in);
  std::size_t off = 0;
  if (!find_marshal_magic(bytes, off)) return "";
  try {
    m::Arena arena;
    // We only need the crc table (the 2nd Marshal value), not the signature: skip
    // the header value by its declared length rather than decoding its whole graph.
    m::skip_value(bytes.data(), bytes.size(), off);
    if (!find_marshal_magic(bytes, off)) return "";
    std::size_t crcs = m::read_value(bytes.data(), bytes.size(), off, arena);  // crc list
    arena.finalize();
    const m::Value& cell = arena[crcs];                             // first cons cell
    if (cell.kind != m::Value::Kind::Block || cell.fields.size() < 1) return "";
    const m::Value& entry = arena[cell.fields[0]];                  // (name, crc option)
    if (entry.kind != m::Value::Kind::Block || entry.fields.size() < 2) return "";
    const m::Value& crcopt = arena[entry.fields[1]];               // None=Int 0 / Some=Block{str}
    if (crcopt.kind != m::Value::Kind::Block || crcopt.fields.empty()) return "";
    return arena[crcopt.fields[0]].str();
  } catch (const std::exception&) {
    return "";
  }
}

// Build the omarshal value graph for one exported value's type.  `id` is a
// per-item counter for the cosmetic type_expr id field; `vars` shares Tvar
// nodes of equal identity (so `'a -> 'a` is one node, used twice).
// NOROWNORM=1: object fields in source order and every variant row in
// Typetexp's hash-descending order (the S550 revert hook).
static bool row_norm_off() {
  static const bool off = cppcaml::dbg_env("NOROWNORM") != nullptr;
  return off;
}

struct TyEmit {
  long long id = -2;
  std::unordered_map<int, o::ValPtr> vars;
  // global module names cited by the signature -> whether they need a real
  // interface CRC (true for a qualified type like Buffer.t; false for a module
  // alias `module M = Unit`, which OCaml imports with CRC=None to avoid a
  // circular dependency, e.g. stdlib.cmi <-> stdlib__List.cmi).
  std::map<std::string, bool>* referenced = nullptr;
  const std::unordered_map<std::string, int>* local_types = nullptr;  // same-sig type -> stamp
  // Engine decl stamp -> emitted Local ident stamp, overlaid through enclosing
  // scopes.  Checked FIRST for a bare Constr carrying an engine_stamp: a value
  // citing an OUTER `t` shadowed by the current module's own `t` must cite the
  // outer decl's ident (Printtyp prints it `t/2`), which the flat name map
  // cannot represent.
  const std::unordered_map<int, int>* engine_types = nullptr;
  const std::unordered_map<std::string, int>* local_modtypes = nullptr;  // same-sig modtype -> stamp
  const std::unordered_map<std::string, int>* local_mods = nullptr;  // visible module -> stamp
  // The name and emitted stamp of the module whose items are being emitted
  // (empty/0 at unit level).  Inside a (non-recursive) module's own signature
  // its name is NOT in scope, so a path head naming it must not self-capture:
  // identifiable.mli's S member `module Set : Set with ..` carries the
  // manifest `t = Set.Make(T).t`, whose head is STDLIB Set, not the member;
  // Make's result member `module T : sig type t = T.t end` cites the functor
  // PARAM T, not itself (stamp-guard in the member-directed lookups).
  std::string self_mod;
  int self_stamp = 0;
  // Visible local modules by simple name, OUTER->INNER, plus each module's
  // directly-declared member names (types/modules/...).  A dotted type head
  // `M.t` resolves to the innermost `M` that actually DECLARES `t`, not just
  // the innermost `M`: pr7402's inner `F.M` shadows the outer `M` but has no
  // `t`, so `M.t` cites the OUTER decl (Printtyp prints it `M/2`).  Null =
  // fall back to local_mods' innermost stamp.
  const std::unordered_map<std::string, std::vector<int>>* mods_by_name = nullptr;
  const std::unordered_map<int, std::set<std::string>>* mod_members = nullptr;
  // Dependent-arrow binders in scope (`(module M : T) -> M.t`): binder name ->
  // the SHARED Ident.Unscoped ValPtr cited by both the Tfunctor node and every
  // `M.t` path in its codomain (ocamlc shares them physically).
  std::unordered_map<std::string, o::ValPtr> unscoped_mods;
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
        path = open_stdlib_root();
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
        return pident_local(ref, it->second);  // Pident(Local)
    return nullptr;
  }
  // The Path.t of a path CONTAINING FUNCTOR APPLICATIONS (`Set.Make(String).t`,
  // `F(A)(B).t`, args possibly nested).  ocamlc stores the applied functor's
  // head as the RAW unit global (Stdlib__Set) -- unlike a plain dotted path,
  // which routes through the Stdlib alias -- while an application-free ARGUMENT
  // keeps the alias form: `Set.Make(String)` is
  // Papply(Pdot(Pident(G Stdlib__Set), "Make"), Pdot(Pident(G Stdlib), "String")).
  // Null when a component can't be placed.
  o::ValPtr module_app_path(const std::string& s) {
    o::ValPtr path;
    std::size_t i = 0;
    while (i < s.size()) {
      std::size_t j = i;
      while (j < s.size() && s[j] != '.' && s[j] != '(') ++j;
      std::string id = s.substr(i, j - i);
      if (!path) {  // head component
        if (id.empty()) return nullptr;
        // A local-module hit must actually DECLARE the next dotted component
        // (member-directed, like type_path's pr7402 rule): S's member Map
        // citing `Set.Make(T).t` means STDLIB Set, not the sibling member
        // `module Set : Set.S` (which has no Make).  Applied heads (`F(A)`)
        // keep the plain hit -- a local functor is exactly that.
        bool local_ok = local_mods && local_mods->count(id);
        int head_stamp = local_ok ? local_mods->at(id) : 0;
        if (local_ok && j < s.size() && s[j] == '.' && mods_by_name &&
            mod_members) {
          std::size_t j2 = j + 1;
          while (j2 < s.size() && s[j2] != '.' && s[j2] != '(') ++j2;
          std::string next = s.substr(j + 1, j2 - j - 1);
          int flat = head_stamp;
          local_ok = false;
          if (auto bn = mods_by_name->find(id); bn != mods_by_name->end())
            for (auto st = bn->second.rbegin(); st != bn->second.rend(); ++st) {
              if (*st == self_stamp) continue;  // own name: not in scope inside own sig
              if (auto mm = mod_members->find(*st);
                  mm != mod_members->end() && mm->second.count(next)) {
                head_stamp = *st;
                local_ok = true;
                break;
              }
            }
          // No local declares the member: only prefer the global route when
          // the head names a real stdlib unit (Set w/o Make -> Stdlib Set);
          // otherwise keep the flat local hit (a bogus global is worse).
          if (!local_ok && flat != self_stamp && global_of(id) == id) {
            head_stamp = flat;
            local_ok = true;
          }
        } else if (local_ok && !self_mod.empty() && id == self_mod &&
                   head_stamp == self_stamp) {
          // Applied/terminal head naming the module being emitted: prefer any
          // non-self same-named module; else fall to the global ladder.
          local_ok = false;
          if (mods_by_name)
            if (auto bn = mods_by_name->find(id); bn != mods_by_name->end())
              for (auto st = bn->second.rbegin(); st != bn->second.rend(); ++st)
                if (*st != self_stamp) {
                  head_stamp = *st;
                  local_ok = true;
                  break;
                }
        }
        if (local_ok) {
          path = pident_local(id, head_stamp);  // Pident(Local)
        } else {
          std::string g = global_of(id);
          if (referenced) (*referenced)[g] = true;
          // A head WRITTEN as the mangled unit (a strengthening manifest the
          // writer generated itself) stays the raw unit global; a source-
          // written pervasive head routes through the Stdlib alias, printing
          // back as written (`Set.Make(X).t` in a declared signature).
          if (g.rfind("Stdlib__", 0) == 0 && id.rfind("Stdlib__", 0) != 0) {
            if (referenced) (*referenced)["Stdlib"] = true;
            path = open_stdlib_root();
            path = o::vblock(1, {path, o::vstr(g.substr(8))});
          } else {
            path = o::vblock(0, {o::vblock(2, {o::vstr(g)})});  // Pident(Global), raw unit
          }
        }
      } else if (!id.empty()) {
        path = o::vblock(1, {path, o::vstr(id)});  // Pdot
      }
      i = j;
      while (i < s.size() && s[i] == '(') {  // application(s) of this prefix
        int depth = 1;
        std::size_t k = i + 1;
        while (k < s.size() && depth) {
          if (s[k] == '(') ++depth;
          if (s[k] == ')') --depth;
          ++k;
        }
        if (depth) return nullptr;  // unbalanced
        o::ValPtr arg = module_app_path(s.substr(i + 1, k - i - 2));
        if (!arg) return nullptr;
        path = o::vblock(2, {path, arg});  // Papply
        i = k;
      }
      if (i < s.size()) {
        if (s[i] != '.') return nullptr;
        ++i;
      }
    }
    return path;
  }
  // The Path.t for a type-ctor name, via the resolution ladder: dotted name ->
  // Pdot chain off the head's compilation-unit global (a Stdlib__ head goes
  // THROUGH the Stdlib alias module -- `Buffer.t` is stored
  // Pdot(Pdot(Pident(Global Stdlib), "Buffer"), "t"), never the mangled unit,
  // and cites Stdlib's CRC too, like ocamlc); predef; sig-local declaration;
  // bare Stdlib-toplevel type (`ref`, resolved through the implicit
  // `open Stdlib`).  Null when the name can't be placed.
  // `prov`: the path object this citation is (cmi.hpp prov_new); it decides
  // which predef Pident / global head / component strings are shared.
  o::ValPtr type_path(const std::string& name, int prov = 0) {
    if (name.find('(') != std::string::npos) return module_app_path(name);
    if (auto dot = name.find('.'); dot != std::string::npos) {
      std::vector<std::string> comps;
      for (std::size_t i = 0, j; i <= name.size(); i = j + 1) {
        j = name.find('.', i);
        if (j == std::string::npos) j = name.size();
        comps.push_back(name.substr(i, j - i));
      }
      // A dependent-arrow binder head (`(module M : T) -> M.t`): Pdot chain
      // off the SHARED Unscoped ident of the enclosing Tfunctor.
      if (auto um = unscoped_mods.find(comps[0]); um != unscoped_mods.end()) {
        o::ValPtr path = o::vblock(0, {o::vblock(4, {um->second})});  // Pident(Unscoped)
        for (std::size_t i = 1; i < comps.size(); ++i)
          path = o::vblock(1, {path, o::vstr(comps[i])});  // Pdot
        return path;
      }
      // A LOCAL module head (`module MP = Gc.Memprof` then MP.allocation):
      // Pdot chain off the sibling's Local ident, like ocamlc -- not a bogus
      // Global that would demand an interface CRC for "MP".
      if (local_mods)
        if (auto lm = local_mods->find(comps[0]); lm != local_mods->end()) {
          int mstamp = lm->second;
          // Pick the innermost visible `M` that DECLARES the next component
          // (`t` in `M.t`): the innermost `M` alone can be the wrong one when a
          // nested module shadows an outer of the same name (pr7402).  The
          // module CURRENTLY BEING EMITTED is excluded -- its own name is not
          // in scope inside its own signature (Make's result member
          // `module T : sig type t = T.t end` cites the functor param T).
          if (comps.size() >= 2 && mods_by_name && mod_members)
            if (auto bn = mods_by_name->find(comps[0]); bn != mods_by_name->end())
              for (auto s = bn->second.rbegin(); s != bn->second.rend(); ++s) {
                if (*s == self_stamp) continue;
                if (auto mm = mod_members->find(*s);
                    mm != mod_members->end() && mm->second.count(comps[1])) {
                  mstamp = *s;
                  break;
                }
              }
          if (mstamp == self_stamp && mods_by_name)
            if (auto bn = mods_by_name->find(comps[0]); bn != mods_by_name->end())
              for (auto s = bn->second.rbegin(); s != bn->second.rend(); ++s)
                if (*s != self_stamp) { mstamp = *s; break; }
          if (mstamp != self_stamp) {
            o::ValPtr path = pident_local(comps[0], mstamp);
            int owner = mstamp;
            // S566: the components the ENVIRONMENT added -- everything but
            // the last `src_dots`, which the source wrote itself -- name
            // declarations OF the module they hang off, so each is that
            // declaration's own ident name string (prefix_idents).
            const std::size_t written =
                comps.size() - 1 - std::min<std::size_t>(
                    comps.size() - 1, prov_info(prov).src_dots);
            for (std::size_t i = 1; i < comps.size(); ++i) {
              int ms = i <= written ? member_stamp(owner, comps[i]) : 0;
              path = o::vblock(1, {path, ms ? member_str(ms, comps[i])
                                            : comp_str(comps[i], prov, i)});  // Pdot
              owner = ms;
            }
            return path;
          }
          // else: the only visible candidate is the module itself -- fall
          // through to the global ladder.
        }
      std::string g = global_of(comps[0]);
      if (referenced) (*referenced)[g] = true;  // a real type ref needs the CRC
      o::ValPtr path;
      // Like module_app_path: a head WRITTEN as the mangled unit (a
      // strengthening manifest the writer generated itself, `type 'a t =
      // 'a Stdlib__Queue.t`) stays the raw unit global; a source-written
      // pervasive head routes through the Stdlib alias.
      if (g.rfind("Stdlib__", 0) == 0 && comps[0].rfind("Stdlib__", 0) != 0) {
        if (referenced) (*referenced)["Stdlib"] = true;
        path = global_head("Stdlib", prov, false);               // Pident(Global Stdlib)
        path = o::vblock(1, {path, alias_str(g.substr(8), prov)});  // Pdot(_, alias member)
      } else {
        path = global_head(g, prov, true);  // Pident(Global head), written
      }
      // S569: everything but the `src_dots` components the SOURCE wrote is a
      // declaration the environment prefixed -- its own ident name (S566).
      const ProvInfo& gpi = prov_info(prov);
      const std::size_t gwritten =
          comps.size() - 1 - std::min<std::size_t>(comps.size() - 1,
                                                   gpi.src_dots);
      for (std::size_t i = 1; i < comps.size(); ++i)
        path = o::vblock(1, {path, env_built(prov) && i <= gwritten
                                       ? unit_member_str(g, (int)i, comps[i])
                                       : comp_str(comps[i], prov, i)});  // Pdot
      return path;
    }
    // A same-sig decl SHADOWS a predefined name: typedtree.mli declares its
    // own `extension_constructor` (also a predef), and every bare cite --
    // including ones earlier in the mutually-recursive and-chain -- means the
    // local decl.  Citing the predef instead gave the field a foreign type
    // identity, so consumers (Tast_iterator) read garbage (bootstrap #13).
    if (local_types && local_types->count(name)) {
      int st = local_types->at(name);
      return pident_local(name, st);  // Pident(Local)
    }
    if (int st = predef_stamp(name))
      return pident_predef(name, st, prov);  // Pident(Predef)
    if (stdlib_toplevel_type(name)) {
      if (referenced) (*referenced)["Stdlib"] = true;
      return o::vblock(1, {global_head("Stdlib", prov, false),
                           env_built(prov)
                               ? unit_member_str("Stdlib", 1, name)
                               : comp_str(name, prov, 1)});  // Pdot
    }
    return nullptr;
  }
  // A Ty node cited twice within one item must marshal as ONE node (omarshal
  // CODE_SHARED back-reference) so the reader sees the sharing: Printtyp
  // names a shared row `as 'a`, and a constrained decl PARAM's citations in
  // labels/manifest print the param's alias name (`{ v : 'a; } constraint
  // ..`) -- two structural copies print unnamed/inline.  Vars are excluded:
  // they share through `vars` by id (several distinct Ty::Var nodes may
  // carry one id).
  // The Types.package record {pack_path; pack_constraints} of a Package Ty
  // (shared by Tpackage and Tfunctor); null when the modtype can't be placed.
  o::ValPtr pack_payload(const Ty& t) {
    o::ValPtr path = mty_path(t.name);
    if (!path) return nullptr;
    std::vector<o::ValPtr> cs;
    for (std::size_t i = 0; i < t.pv_tags.size() && i < t.args.size(); ++i) {
      // the constraint's type name, split on dots into a string list
      std::vector<o::ValPtr> comps;
      const std::string& n = t.pv_tags[i];
      for (std::size_t s = 0, e; s <= n.size(); s = e + 1) {
        e = n.find('.', s);
        if (e == std::string::npos) e = n.size();
        comps.push_back(o::vstr(n.substr(s, e - s)));
        if (e == n.size()) break;
      }
      cs.push_back(o::vblock(0, {o::vlist(comps), emit(t.args[i])}));
    }
    return o::vblock(0, {path, cs.empty() ? o::vint(0) : o::vlist(cs)});
  }
  // Does the type cite a module path headed by binder `b` (`b.t`, `b.P.t`)?
  static bool mentions_mod_head(const TyPtr& t, const std::string& b) {
    if (!t) return false;
    if (t->k == Ty::Constr && t->name.size() > b.size() &&
        t->name.compare(0, b.size(), b) == 0 && t->name[b.size()] == '.')
      return true;
    for (auto& a : t->args)
      if (mentions_mod_head(a, b)) return true;
    return false;
  }
  // Stamps for writer-invented Unscoped binder idents: far above any sig item
  // stamp so they never collide within one signature.
  static int next_unscoped_stamp() {
    static int s = 90000000;
    return ++s;
  }
  std::unordered_map<const Ty*, o::ValPtr> shared_nodes;
  o::ValPtr emit(const TyPtr& t) {
    if (t->k == Ty::Var) return emit_fresh(t);
    if (auto it = shared_nodes.find(t.get()); it != shared_nodes.end())
      return it->second;
    // Register a type_expr shell BEFORE emitting the children so a CYCLIC Ty
    // graph (a recursive row/object, `< bark : 'a -> unit > t as 'a`) closes
    // back onto this node; omarshal's seen-map turns the loop into a
    // CODE_SHARED back-reference, which is exactly how ocamlc stores it.
    o::ValPtr shell = texpr(o::vint(0));
    shared_nodes[t.get()] = shell;
    o::ValPtr res = emit_fresh(t);
    if (res->k != o::Value::Block) {  // degenerate fallback, not a type_expr
      shared_nodes[t.get()] = res;
      return res;
    }
    shell->fields = res->fields;
    return shell;
  }
  o::ValPtr emit_fresh(const TyPtr& t) {
    switch (t->k) {
      case Ty::Var: {
        if (auto it = vars.find(t->var); it != vars.end()) return it->second;
        // Tunivar for a poly field's `'a.` binder, Tvar otherwise
        o::ValPtr te = texpr(
            t->var_name.empty()
                ? var_none_desc(t->univar)
                : o::vblock(t->univar ? 7 : 0,
                            {o::vblock(0, {o::vstr(t->var_name)})}));  // Some
        vars[t->var] = te;
        return te;
      }
      case Ty::Poly: {
        // Tpoly(body, [the quantified Tunivar nodes]) -- body emitted first so
        // its univars register in `vars` and can be cited (shared) here.
        o::ValPtr body = emit(t->args[0]);
        std::vector<o::ValPtr> us;
        for (int pid : t->poly_ids)
          if (auto it = vars.find(pid); it != vars.end()) us.push_back(it->second);
        return texpr(o::vblock(8, {body, us.empty() ? o::vint(0) : o::vlist(us)}));
      }
      case Ty::Constr: {
        // A qualified name (`Buffer.t`, `A.Inner.t`) emits a Tconstr whose path
        // is Pdot(...Pdot(Pident(Global head), mid)..., typename), with `head`
        // resolved to its compilation-unit global; the cited unit is recorded so
        // write_cmi can list it (with its CRC) among the imports.  A predefined
        // name emits Pident(Predef) with the predef.ml stamp.  Anything else (a
        // bare user/local type we can't yet place) degrades to an opaque Tvar --
        // valid, just over-general.
        o::ValPtr path;
        if (t->engine_stamp && engine_types &&
            t->name.find('.') == std::string::npos)
          if (auto es = engine_types->find(t->engine_stamp);
              es != engine_types->end())
            path = pident_local(t->name, es->second);  // Pident(Local)
        if (!path) path = type_path(t->name, t->prov);
        if (!path) return texpr(var_none_desc(false));  // unknown -> Tvar None
        std::vector<o::ValPtr> as;
        for (auto& a : t->args) as.push_back(emit(a));
        auto abbrev = o::vblock(0, {o::vint(0)});  // ref Mnil
        return texpr(o::vblock(3, {path, as.empty() ? o::vint(0) : o::vlist(as), abbrev}));  // Tconstr
      }
      case Ty::Arrow: {
        // arg_label = Nolabel (int 0) | Labelled of string (block tag 0)
        //           | Optional of string (block tag 1)
        o::ValPtr lbl = t->label_kind == 1 ? o::vblock(0, {o::vstr(t->label)})
                      : t->label_kind == 2 ? o::vblock(1, {o::vstr(t->label)})
                      : o::vint(0) /*Nolabel*/;
        // A DEPENDENT arrow `(module M : T) -> .. M.t ..` (the named unpack
        // binder escapes into the codomain) is Tfunctor(lbl, Unscoped M,
        // package, cod), and the codomain's `M.t` paths cite the SAME
        // Unscoped ident.  A binder that does not escape stays a plain
        // Tarrow(Tpackage) -- exactly ocamlc's split.
        if (t->args[0]->k == Ty::Package && !t->args[0]->binder.empty() &&
            mentions_mod_head(t->args[1], t->args[0]->binder)) {
          if (o::ValPtr package = pack_payload(*t->args[0])) {
            const std::string& b = t->args[0]->binder;
            o::ValPtr uns = o::vblock(0, {o::vblock(0, {o::vblock(0,
                {o::vstr(b), o::vint(next_unscoped_stamp())})})});
            auto prev = unscoped_mods.find(b);
            o::ValPtr saved = prev != unscoped_mods.end() ? prev->second : nullptr;
            unscoped_mods[b] = uns;
            o::ValPtr c = emit(t->args[1]);
            if (saved) unscoped_mods[b] = saved; else unscoped_mods.erase(b);
            return texpr(o::vblock(10, {lbl, uns, package, c}));  // Tfunctor
          }
        }
        // In this trunk a Tarrow's DOMAIN is wrapped in Tpoly(ty, []) (to allow
        // first-class-poly arguments); the codomain stays bare.  OCaml asserts
        // (btype.tpoly_get_mono) if the argument isn't a Tpoly.
        o::ValPtr inner = emit(t->args[0]);
        if (t->label_kind == 2 &&
            !(t->args[0]->k == Ty::Constr && t->args[0]->name == "option")) {
          // An OPTIONAL argument's stored domain is `d option` (the printer
          // strips it back to `?x:d`; a bare domain prints `?x:<hidden>`).
          // Typecore.type_option cites Predef.path_option: ONE block per
          // compile (max_arity's 133 optional parameters share it).
          auto opath = node_id_off()
                           ? o::vblock(0, {o::vblock(3, {o::vstr("option"), o::vint(12)})})
                           : pident_predef("option", 12);
          inner = texpr(o::vblock(3, {opath, o::vlist({inner}),
                                      o::vblock(0, {o::vint(0)})}));  // Tconstr option
        }
        // An already-poly domain (`('a. 'a -> 'a) -> ..`) is its own Tpoly.
        o::ValPtr dom = t->args[0]->k == Ty::Poly
                            ? inner
                            : texpr(o::vblock(8, {inner, o::vint(0) /*[]*/}));  // Tpoly
        o::ValPtr c = emit(t->args[1]);
        // The commutable (see infer.hpp's Type::commu): `Cok`, or the
        // `Cvar {Cunknown}` cell Btype.copy_commu mints for every copy of an
        // unresolved one.
        o::ValPtr commu = t->commu_var && !commu_off()
                              ? o::vblock(0, {o::vint(1) /*Cunknown*/})
                              : o::vint(0) /*Cok*/;
        return texpr(o::vblock(1, {lbl, dom, c, commu}));  // Tarrow
      }
      case Ty::Tuple: {
        std::vector<o::ValPtr> elems;
        for (std::size_t k = 0; k < t->args.size(); ++k) {
          // Labeled-tuple component labels ride pv_tags ("" = unlabeled).
          o::ValPtr lbl =
              k < t->pv_tags.size() && !t->pv_tags[k].empty()
                  ? o::vblock(0, {o::vstr(t->pv_tags[k])})  // Some label
                  : o::vint(0);                             // None
          elems.push_back(o::vblock(0, {lbl, emit(t->args[k])}));  // (label,ty)
        }
        return texpr(o::vblock(2, {o::vlist(elems)}));  // Ttuple of (so * te) list
      }
      case Ty::Variant: {
        // A polymorphic-variant row.  ocamlc stores row_fields sorted by tag
        // HASH descending (Btype.hash_variant -- the merge-joins in
        // Ctype.subtype/unify pair tags positionally over hash-sorted rows,
        // so any other order silently mispairs: patterns.cmi's
        // `Half_simple.pattern :> General.pattern` width coercion was
        // rejected with "does not allow tag(s) `Alias, `Var" under
        // reverse-ALPHABETICAL order, which coincides with hash order only
        // for small rows); an exact row's row_more is Tnil, an open/upper
        // one's a Tvar.  Fields are RFpresent(arg option), except an upper
        // `[<` row's non-present tags which are RFeither{no_arg; arg_type;
        // matched=false; ext=ref RFnone}.
        // row_desc = { row_fields; row_more; row_closed; row_fixed; row_name }.
        auto hash_variant = [](const std::string& s) -> long long {
          unsigned long long accu = 0;
          for (unsigned char c : s) accu = 223 * accu + c;
          long long r = (long long)(accu & ((1ULL << 31) - 1));
          if (r > 0x3FFFFFFF) r -= (1LL << 31);
          return r;
        };
        std::vector<std::size_t> ord(t->pv_tags.size());
        for (std::size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) {
          return hash_variant(t->pv_tags[a]) > hash_variant(t->pv_tags[b]);
        });
        std::unordered_set<std::string> present(t->pv_present.begin(),
                                                t->pv_present.end());
        std::vector<o::ValPtr> fields;
        for (std::size_t i : ord) {
          TyPtr arg = i < t->args.size() ? t->args[i] : nullptr;
          bool conj = i < t->pv_conj.size() && t->pv_conj[i];
          o::ValPtr rf;
          if (t->row_kind == 1 && !present.count(t->pv_tags[i])) {
            rf = o::vblock(1, {o::vint((!arg || conj) ? 1 : 0) /*no_arg*/,
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
                             : texpr(var_none_desc(false));         // Tvar None
        // A named row bound (`[< int u]`): row_name = Some(path, args) -- the
        // printer shows `[< int u > `A ]` from the name instead of the raw tags.
        o::ValPtr rname = o::vint(0);
        if (!t->row_name.empty())
          if (o::ValPtr rp = type_path(t->row_name)) {
            std::vector<o::ValPtr> ra;
            for (auto& a : t->row_name_args) ra.push_back(emit(a));
            rname = o::vblock(0, {o::vblock(0, {rp, ra.empty() ? o::vint(0)
                                                               : o::vlist(ra)})});
          }
        o::ValPtr rd = o::vblock(0, {fields.empty() ? o::vint(0) : o::vlist(fields),
                                     more, o::vint(t->row_kind != 0 ? 1 : 0) /*row_closed*/,
                                     o::vint(0) /*row_fixed=None*/, rname});
        return texpr(o::vblock(6, {rd}));  // Tvariant of row_desc
      }
      case Ty::Object: {
        // A structural object type `< m1 : t1; m2 : t2 >`:
        // Tobject(Tfield(m1, FKpublic, Tpoly(t1,[]), ... Tnil), ref None).
        // Each method type is Tpoly-wrapped (ocamlc stores even monomorphic
        // methods as Tpoly(ty, [])); the row terminates in Tnil (closed) or a
        // Tvar for an OPEN row (`< m : t; .. >`, row_kind 0).
        // The fields are chained in ascending NAME order: Typetexp's
        // transl_fields folds them out of a String.Map, and an inferred
        // object's row is sorted by Ctype.normalize_type (S550).
        std::vector<std::size_t> ord(t->pv_tags.size());
        for (std::size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        if (!row_norm_off())
          std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) {
            return t->pv_tags[a] < t->pv_tags[b];
          });
        o::ValPtr tailvar;  // the OPEN row's terminating Tvar (shared with nm)
        o::ValPtr row = t->row_kind == 0
                            ? (tailvar = texpr(var_none_desc(false)))
                            : texpr(o::vint(0));                 // Tnil (a full type_expr node)
        for (std::size_t k = ord.size(); k-- > 0;) {
          std::size_t i = ord[k];
          o::ValPtr mty = texpr(o::vblock(8, {emit(t->args[i]), o::vint(0)}));  // Tpoly(ty,[])
          row = texpr(o::vblock(5, {o::vstr(t->pv_tags[i]), o::vint(1) /*FKpublic*/,
                                    mty, row}));  // Tfield
        }
        // A NAMED open row (`#c`): Tobject's name = ref Some(c, rowvar ::
        // params).  Printtyp prints `#c` while the first arg is still a Tvar
        // (the reader's normalize resets the name once it instantiates).
        o::ValPtr nm = o::vblock(0, {o::vint(0)});  // ref None
        if (!t->row_name.empty() && tailvar)
          if (o::ValPtr rp = type_path(t->row_name)) {
            std::vector<o::ValPtr> na{tailvar};
            for (auto& a : t->row_name_args) na.push_back(emit(a));
            // ref (Some (p, args)): ref cell -> Some -> the (p, list) pair
            nm = o::vblock(0, {o::vblock(0, {o::vblock(0, {rp, o::vlist(na)})})});
          }
        return texpr(o::vblock(4, {row, nm}));  // Tobject(row, name)
      }
      case Ty::Package: {
        if (o::ValPtr package = pack_payload(*t))
          return texpr(o::vblock(9, {package}));  // Tpackage
        return texpr(var_none_desc(false));  // unplaceable -> Tvar None
      }
    }
    return o::vint(0);
  }
};

// Location.none = Warnings.ghost_loc_in_file "_none_" = Lexing.dummy_pos with
// pos_fname REPLACED (warnings.ml:716) -- not dummy_pos itself, whose pos_fname
// is "".  Both ends are the same physical position, so the marshaller shares
// the second.  NONONELOC restores the "" we wrote before.
// {pos_fname="_none_"; pos_lnum=0; pos_bol=0; pos_cnum=-1}
o::ValPtr none_pos() {
  return o::vblock(0, {o::vstr(cppcaml::dbg_env("NONONELOC") ? "" : "_none_"),
                       o::vint(0), o::vint(0), o::vint(-1)});
}
o::ValPtr loc_none() {  // Location.none = {loc_start; loc_end; loc_ghost=true}
  if (!no_share() && g_share.none) return g_share.none;
  auto p = none_pos();
  auto l = o::vblock(0, {p, p, o::vint(1)});
  if (!no_share()) g_share.none = l;
  return l;
}
// The source filenames for the cmi being written: [0] = the source path (file_id
// 0), [k] = the k-th `# N "file"` directive.  Set by write_cmi; read by emit_loc
// to resolve a position's file_id to its pos_fname.
static std::vector<std::string> g_cmi_src_files;
o::ValPtr emit_pos(const cmiw::WPos& p) {  // Lexing.position
  // A foreign pos_fname (read from a dependency's cmi) is authoritative; else
  // resolve the file_id through this unit's source-file table.
  const std::string& fn = !p.fname.empty() ? p.fname
      : (p.file_id >= 0 && (std::size_t)p.file_id < g_cmi_src_files.size())
                              ? g_cmi_src_files[p.file_id] : g_cmi_src_files.empty() ? "" : g_cmi_src_files[0];
  // A position is the lexer's own record for a token boundary, so every
  // location that starts or ends there cites the SAME one: `type t = A` ends
  // at char 10 and so does its constructor, and ocamlc's .cmi writes the
  // second as a back-reference.
  if (no_share())
    return o::vblock(0, {shared_str(fn), o::vint(p.lnum), o::vint(p.bol),
                         o::vint(p.cnum)});
  auto& v = g_share.poss[fn + ":" + std::to_string(p.lnum) + ":" +
                         std::to_string(p.bol) + ":" + std::to_string(p.cnum)];
  if (!v)
    v = o::vblock(0, {shared_str(fn), o::vint(p.lnum), o::vint(p.bol),
                      o::vint(p.cnum)});
  return v;
}
o::ValPtr emit_loc(const cmiw::Loc& l) {  // Location.t
  if (l.ghost) return loc_none();
  return o::vblock(0, {emit_pos(l.start), emit_pos(l.end), o::vint(0) /*loc_ghost=false*/});
}
// The same declaration's location, wherever it is re-exported (S574).
o::ValPtr emit_loc(const cmiw::Loc& l, const std::string& key) {
  if (key.empty()) return emit_loc(l);
  auto& v = g_share.decl_locs[key];
  if (!v) v = emit_loc(l);
  return v;
}
// A payload expression (S589): `{pexp_desc; pexp_loc; pexp_loc_stack = [];
// pexp_attributes = []}`.  Pexp_ident is tag 0 (`{txt = Lident s; loc}`),
// Pexp_constant tag 1 (`{pconst_desc; pconst_loc}`), Pexp_apply tag 4.
o::ValPtr emit_pexpr(const cmiw::PExpr& e) {
  o::ValPtr d;
  if (e.k == 'I') {
    d = o::vblock(0, {o::vblock(0, {o::vblock(0, {o::vstr(e.s)}), emit_loc(e.cloc)})});
  } else if (e.k == 'S' || e.k == 'N') {
    auto opt = [](o::ValPtr x) { return o::vblock(0, {x}); };
    o::ValPtr cd =
        e.k == 'S'
            ? o::vblock(2, {o::vstr(e.s), emit_loc(e.sloc),
                            e.delim ? opt(o::vstr(*e.delim)) : o::vint(0)})
            : o::vblock(0, {o::vstr(e.s),
                            e.suffix ? opt(o::vint((unsigned char)*e.suffix)) : o::vint(0)});
    d = o::vblock(1, {o::vblock(0, {cd, emit_loc(e.cloc)})});
  } else {
    std::vector<o::ValPtr> args;
    for (std::size_t i = 1; i < e.kids.size(); ++i)
      args.push_back(o::vblock(0, {o::vint(0) /*Nolabel*/, emit_pexpr(e.kids[i])}));
    d = o::vblock(4, {emit_pexpr(e.kids[0]), o::vlist(args)});
  }
  return o::vblock(0, {d, emit_loc(e.loc), o::vint(0), o::vint(0)});
}
// A declaration's attribute list (S577): each `{attr_name = {txt; loc};
// attr_payload = PStr []; attr_loc}`, in source order.  A payload's one item
// is `{pstr_desc = Pstr_eval (e, []); pstr_loc = e.pexp_loc}` (S589).
o::ValPtr emit_attrs(const std::vector<cmiw::Attr>& as) {
  std::vector<o::ValPtr> v;
  for (auto& a : as) {
    auto name = o::vblock(0, {o::vstr(a.name), emit_loc(a.name_loc)});
    o::ValPtr str = o::vint(0);
    if (a.pl) {
      auto e = emit_pexpr(*a.pl);
      str = o::vlist({o::vblock(0, {o::vblock(0, {e, o::vint(0)}), e->fields[1]})});
    }
    v.push_back(o::vblock(0, {name, o::vblock(0, {str}), emit_loc(a.loc)}));
  }
  return v.empty() ? o::vint(0) : o::vlist(v);
}
// A label's / constructor's / module's / module type's / extension
// constructor's attribute list (S588): `[]` when not modelled.
o::ValPtr emit_attrs(const std::optional<std::vector<cmiw::Attr>>& as) {
  return as && !attrpos_off() ? emit_attrs(*as) : o::vint(0);
}
// Shape.Uid.t marshal repr.  Constant ctor Internal -> immediate 0.  Non-constant
// ctors in declaration order: Compilation_unit(0), Item(1), Local_opaque_item(2),
// Predef(3).  Item's `from` is Unit_info.intf_or_impl = Intf(0) | Impl(1).
o::ValPtr emit_uid_raw(const cmiw::Uid& u) {
  switch (u.k) {
    case cmiw::Uid::Item:
      return o::vblock(1, {shared_str(u.unit), o::vint(u.id),
                           o::vint(u.intf ? 0 : 1)});
    case cmiw::Uid::CompUnit: return o::vblock(0, {shared_str(u.unit)});
    case cmiw::Uid::Predef:   return o::vblock(3, {shared_str(u.unit)});
    case cmiw::Uid::Internal: break;
  }
  return o::vint(0);  // Internal
}
// One Uid block per declaration (S574): `Subst` copies the uid physically, so
// every signature that re-exports a declaration cites the same block.
o::ValPtr emit_uid(const cmiw::Uid& u, const std::string& key = "") {
  if (key.empty()) return emit_uid_raw(u);
  auto& v = g_share.decl_uids[key];
  if (!v) v = emit_uid_raw(u);
  return v;
}
}  // namespace

// Visible local modules threaded through nested signatures so a dotted type
// head resolves to the module that truly declares the member (see TyEmit).
struct ModScope {
  std::unordered_map<std::string, std::vector<int>> by_name;  // name -> stamps, outer->inner
  std::unordered_map<int, std::set<std::string>> members;     // stamp -> declared member names
};

// The member names a module's signature declares directly (types, submodules,
// module types, classes) -- the components a dotted path off it can name.
static std::set<std::string> module_member_names(const std::vector<SigItem>& sub) {
  std::set<std::string> names;
  for (auto& it : sub)
    if (it.k == SigItem::Type || it.k == SigItem::Module ||
        it.k == SigItem::Modtype || it.k == SigItem::Class)
      names.insert(it.name);
  return names;
}

// Marshal a list of signature items (recursive: a submodule's items nest under
// Mty_signature).  `stamp` is a counter shared across the whole cmi so every
// local ident is unique (a value's type referencing a same-module `type t`
// must cite that decl's exact stamp).
static std::vector<o::ValPtr> emit_sig_items(const std::vector<SigItem>& items,
                                             std::map<std::string, bool>& referenced,
                                             int& stamp,
                                             const std::unordered_map<std::string, int>* outer_types = nullptr,
                                             const std::unordered_map<std::string, int>* outer_modtypes = nullptr,
                                             const std::unordered_map<std::string, int>* outer_mods = nullptr,
                                             const std::unordered_map<int, int>* outer_eng = nullptr,
                                             const ModScope* outer_modscope = nullptr,
                                             const std::vector<const std::unordered_map<std::string, int>*>* outer_scopes = nullptr,
                                             const std::string& self_name = "",
                                             int self_stamp = 0,
                                             int owner_stamp = 0);
static std::vector<o::ValPtr> emit_sig_items(const std::vector<SigItem>& items,
                                             std::map<std::string, bool>& referenced,
                                             int& stamp,
                                             const std::unordered_map<std::string, int>* outer_types,
                                             const std::unordered_map<std::string, int>* outer_modtypes,
                                             const std::unordered_map<std::string, int>* outer_mods,
                                             const std::unordered_map<int, int>* outer_eng,
                                             const ModScope* outer_modscope,
                                             const std::vector<const std::unordered_map<std::string, int>*>* outer_scopes,
                                             const std::string& self_name,
                                             int self_stamp,
                                             int owner_stamp) {
  // Pre-pass: give every item its stamp up front and record the local type
  // names, so a value emitted before/after a type can still cite it by stamp.
  std::vector<int> item_stamp(items.size());
  std::unordered_map<std::string, int> local_types, local_modtypes, local_mods;
  std::unordered_map<std::string, int> local_classes;  // class name -> CLASS ident
  for (std::size_t i = 0; i < items.size(); ++i) {
    item_stamp[i] = stamp;
    // A Class takes THREE idents (class, ghost class type, ghost object type
    // `type c` -- what value types cite as `c`); a `class type` decl TWO.
    int nid = items[i].k == SigItem::Class ? (items[i].class_is_type ? 2 : 3) : 1;
    stamp += nid;
    if (items[i].k == SigItem::Type) local_types[items[i].name] = item_stamp[i];
    if (items[i].k == SigItem::Class) local_types[items[i].name] = item_stamp[i] + nid - 1;
    if (items[i].k == SigItem::Class) local_classes[items[i].name] = item_stamp[i];
    if (items[i].k == SigItem::Modtype) local_modtypes[items[i].name] = item_stamp[i];
    if (items[i].k == SigItem::Module) local_mods[items[i].name] = item_stamp[i];
    // S566: a dotted citation of this member off the enclosing module cites
    // THIS ident's name string (a class takes three idents built over one
    // shared name of their own, so it is left out).
    if (owner_stamp && (items[i].k == SigItem::Type ||
                        items[i].k == SigItem::Module))
      g_share.mstamps[std::to_string(owner_stamp) + ":" + items[i].name] =
          item_stamp[i];
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
  // Sibling/enclosing MODULES, same overlay -- a functor-application manifest
  // (`type t = Set.Make(Loc).t`) cites a local argument module by its stamp.
  std::unordered_map<std::string, int> visible_mod;
  if (outer_mods) visible_mod = *outer_mods;
  for (auto& [n, s] : local_mods) visible_mod[n] = s;
  // Module scope for member-directed head resolution: inherit the enclosing
  // one, then append THIS level's modules (outer entries stay first, so a
  // name's stamp vector runs outer->inner).
  ModScope modscope;
  if (outer_modscope) modscope = *outer_modscope;
  for (std::size_t i = 0; i < items.size(); ++i)
    if (items[i].k == SigItem::Module) {
      modscope.by_name[items[i].name].push_back(item_stamp[i]);
      modscope.members[item_stamp[i]] = module_member_names(items[i].sub);
    }
  // Engine-stamped type decls visible here (outer + this level's own; stamps
  // are globally unique so there is no shadowing among KEYS -- shadowing is
  // exactly what the two distinct entries express).
  std::unordered_map<int, int> visible_eng;
  if (outer_eng) visible_eng = *outer_eng;
  for (std::size_t i = 0; i < items.size(); ++i)
    if (items[i].k == SigItem::Type && items[i].engine_stamp)
      visible_eng[items[i].engine_stamp] = item_stamp[i];
  // Decl-order visibility for TYPE declarations: signature items scope in
  // order -- a type's manifest/body sees EARLIER siblings and its own rec
  // group (`type t .. and u ..`), but not later decls, and under `nonrec`
  // not the group being declared either.  shape.mli's `module Map : sig
  // type shape = t  type nonrec t = t Item.Map.t end` cites the ENCLOSING
  // Shape.t in both -- the flat `visible` map self-captured them to Map's
  // own t.  Values keep the flat map (ocamlc binds a val's citations against
  // the final sig, and inferred .ml items may sit before their type).
  // type_vis_end[i] = index below which local type/class names are in scope
  // for item i's own emission; items.size() = no restriction (flat map).
  std::vector<std::size_t> type_vis_end(items.size(), items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].k != SigItem::Type || items[i].rec_status == 0) continue;
    std::size_t h = i;  // group head (rec_status 1 or -1)
    while (h > 0 && items[h].rec_status == 2 && items[h].k == SigItem::Type)
      --h;
    std::size_t e = h + 1;  // one past the group's last member
    while (e < items.size() && items[e].k == SigItem::Type &&
           items[e].rec_status == 2)
      ++e;
    // nonrec: the group's own names are NOT visible in its bodies.
    type_vis_end[i] = items[h].rec_status == -1 ? h : e;
  }
  std::map<std::size_t, std::unordered_map<std::string, int>> ordvis_cache;
  auto ordered_types = [&](std::size_t end) {
    auto f = ordvis_cache.find(end);
    if (f == ordvis_cache.end()) {
      std::unordered_map<std::string, int> m;
      if (outer_types) m = *outer_types;
      for (std::size_t j = 0; j < end; ++j) {
        if (items[j].k == SigItem::Type) m[items[j].name] = item_stamp[j];
        if (items[j].k == SigItem::Class)
          m[items[j].name] = item_stamp[j] + (items[j].class_is_type ? 1 : 2);
      }
      f = ordvis_cache.emplace(end, std::move(m)).first;
    }
    return &f->second;
  };
  std::map<std::size_t, std::unordered_map<int, int>> ordeng_cache;
  auto ordered_eng = [&](std::size_t end) {
    auto f = ordeng_cache.find(end);
    if (f == ordeng_cache.end()) {
      std::unordered_map<int, int> m;
      if (outer_eng) m = *outer_eng;
      for (std::size_t j = 0; j < end; ++j)
        if (items[j].k == SigItem::Type && items[j].engine_stamp)
          m[items[j].engine_stamp] = item_stamp[j];
      f = ordeng_cache.emplace(end, std::move(m)).first;
    }
    return &f->second;
  };
  // Scope stack of per-level visible-types maps (outermost..this level): a
  // `with type`-spliced manifest resolves its bare names in the scope where
  // the constraint was WRITTEN (with_scope_skip levels up), never against the
  // refined signature's own same-named decls (`Map.S with type key = t` cites
  // the enclosing t, not Map.S's own abstract t).
  std::vector<const std::unordered_map<std::string, int>*> scopes;
  if (outer_scopes) scopes = *outer_scopes;
  scopes.push_back(&visible);
  // The Path.t for a named modtype reference: a dotted name goes through the
  // head unit's global (importing it), a bare one through the visible map's
  // Local stamp.  A dotted head that is a VISIBLE LOCAL MODULE -- a functor's
  // own parameter (`(X : S) -> X.T`, extra_mods) or a sibling module --
  // resolves by stamp instead (Pdot(Pident(Local X), "T")).  Null when the
  // name can't be placed (caller falls back to the inlined signature).
  auto modtype_path = [&](const std::string& ref,
                          const std::unordered_map<std::string, int>*
                              extra_mods = nullptr) -> o::ValPtr {
    if (auto dot = ref.find('.'); dot != std::string::npos) {
      std::string h = ref.substr(0, dot);
      for (auto* m : {extra_mods, (const std::unordered_map<std::string, int>*)&visible_mod})
        if (m)
          if (auto f = m->find(h); f != m->end()) {
            o::ValPtr path = pident_local(h, f->second);  // Pident(Local)
            for (std::size_t pos = dot; pos != std::string::npos;) {
              std::size_t nd = ref.find('.', pos + 1);
              path = o::vblock(1, {path, o::vstr(ref.substr(pos + 1,
                  nd == std::string::npos ? std::string::npos : nd - pos - 1))});  // Pdot
              pos = nd;
            }
            return path;
          }
      std::string head = global_of(ref.substr(0, dot));
      referenced.emplace(head, true);
      o::ValPtr path;
      if (head.rfind("Stdlib__", 0) == 0) {
        // pervasive head through the Stdlib alias: `(Elem : Map.OrderedType)`
        // is Pdot(Pdot(Pident(Global Stdlib), "Map"), "OrderedType")
        referenced.emplace("Stdlib", true);
        path = open_stdlib_root();
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
    if (auto it = visible_mt.find(ref); it != visible_mt.end())
      return pident_local(ref, it->second);  // Pident(Local)
    return nullptr;
  };
  // Build the Mty_functor value for a functor SigItem (is_functor): the curried
  // parameter chain (innermost last) wrapping the result signature.  Shared by a
  // functor MODULE decl (`module Make (Ord : _) : S`) and a functor MODULE TYPE
  // decl (`module type S1 = (S0 -> S0') -> S0`, shape_size_blowup) -- both
  // marshal to the identical Mty_functor.
  auto emit_functor_mty = [&](const SigItem& it) -> o::ValPtr {
    // Mty_functor(Named(Some p1, ..), Mty_functor(Named(Some p2, ..), ..
    // Mty_signature(result))): the curried parameter chain, innermost last.
    // A generative parameter is Unit (the int constructor 0); otherwise
    // Named(Some id, <param sig>).  Params are visible to LATER param sigs and
    // the result (a partial-application manifest `Outcome.Make(IntT)(N).t` cites
    // param N by its Local ident).
    std::unordered_map<std::string, int> visible_mod_body = visible_mod;
    // Params join the module scope of the body/result too (member-directed
    // head resolution): Make's result member `module T : sig type t = T.t
    // end` must see the PARAM T (which declares t) past its own name.
    ModScope modscope_body = modscope;
    auto mk_param = [&](bool unit, const std::string& pname,
                        const std::vector<SigItem>& psig_items,
                        const std::string& ref,
                        const std::vector<SigItem>* pfunc = nullptr) -> o::ValPtr {
      if (unit) return o::vint(0);  // functor_parameter = Unit
      // An anonymous parameter (`sig .. end -> X` or `functor (_ : S)`) has
      // no binder: ocamlc stores Named(None, <sig>), which Printtyp collapses
      // to the arrow form `<sig> -> ..` (no `( : ..)` wrapper).
      o::ValPtr name_opt;
      if (pname.empty()) {
        name_opt = o::vint(0);  // None
      } else {
        int pstamp = stamp++;
        auto pident = ident_val(0, pname, pstamp);  // Ident.Local
        visible_mod_body[pname] = pstamp;
        modscope_body.by_name[pname].push_back(pstamp);
        modscope_body.members[pstamp] = module_member_names(psig_items);
        name_opt = o::vblock(0, {pident});  // Some
      }
      // A NAMED param modtype (`(K : Key)`) emits Mty_ident(Key) like
      // ocamlc; the inlined signature is the fallback.
      o::ValPtr psig;
      // A HIGHER-ORDER param (`(F : (X : S) -> T)`): emit the carried
      // functor Module item through this very function (recursion) and
      // reuse its module_declaration's md_type as the parameter type.
      // Sig_module = block(3, {ident, presence, md, ..}); md_type = md[0].
      if (pfunc && !pfunc->empty()) {
        auto emitted = emit_sig_items({(*pfunc)[0]}, referenced, stamp,
                                      &visible, &visible_mt, &visible_mod_body, &visible_eng, &modscope, &scopes);
        if (emitted.size() == 1 && emitted[0]->fields.size() >= 3 &&
            !emitted[0]->fields[2]->fields.empty())
          psig = emitted[0]->fields[2]->fields[0];
      }
      if (!psig && !ref.empty())
        if (o::ValPtr mp = modtype_path(ref, &visible_mod_body)) psig = o::vblock(0, {mp});  // Mty_ident
      if (!psig)
        psig = o::vblock(1, {o::vlist(emit_sig_items(psig_items, referenced, stamp, &visible, &visible_mt, &visible_mod_body, &visible_eng, &modscope, &scopes))});  // Mty_signature
      return o::vblock(0, {name_opt, psig});  // Named(name_opt, <param sig>)
    };
    std::vector<o::ValPtr> params;
    params.push_back(mk_param(it.functor_unit, it.functor_param, it.param_sig,
                              it.functor_param_ref, &it.param_functor));
    for (std::size_t p = 0; p < it.more_param_names.size(); ++p)
      params.push_back(mk_param(p < it.more_param_units.size() && it.more_param_units[p],
                                it.more_param_names[p],
                                p < it.more_param_sigs.size() ? it.more_param_sigs[p]
                                                              : std::vector<SigItem>{},
                                p < it.more_param_refs.size() ? it.more_param_refs[p]
                                                              : std::string()));
    // A NAMED result modtype (`module F () : Ret`) is stored Mty_ident;
    // the resolved items are the fallback.
    o::ValPtr body;
    if (!it.functor_result_ref.empty())
      if (o::ValPtr rp = modtype_path(it.functor_result_ref, &visible_mod_body))
        body = o::vblock(0, {rp});  // Mty_ident
    if (!body)
      body = o::vblock(1, {o::vlist(emit_sig_items(it.sub, referenced, stamp, &visible, &visible_mt, &visible_mod_body, &visible_eng, &modscope_body, &scopes))});  // Mty_signature(result)
    for (auto p = params.rbegin(); p != params.rend(); ++p)
      body = o::vblock(2, {*p, body});  // Mty_functor
    return body;
  };
  std::vector<o::ValPtr> sig;
  // One `type t += A | B` extension: every constructor's ext_type_path and
  // ext_type_params are the extension's own (transl_type_extension builds
  // them once), so a Text_next item cites its Text_first's blocks.
  std::vector<o::ValPtr> ext_group_params;
  // One type_expr per shared Ty node (and per variable id) across the
  // signature's sigwide values: their bridge shares nodes and numbers
  // variables together (SigItem::sigwide), so the memo carries over.
  std::unordered_map<int, o::ValPtr> sw_vars;
  std::unordered_map<const Ty*, o::ValPtr> sw_nodes;
  // A re-exported `and` member cited before its own item (`t = unit -> u`
  // ahead of `u`) is still `Ident.rename` of the original: its name string
  // is the original's too, so its ident block is made up front.
  if (!fwdidstr_off())
    for (std::size_t i = 0; i < items.size(); ++i)
      if (items[i].k == SigItem::Type)
        ident_val(0, items[i].name, item_stamp[i], 0,
                  decl_name_str(items[i].name,
                                decl_key(items[i].name, items[i].loc, items[i].uid)));
  for (std::size_t i = 0; i < items.size(); ++i) {
    const SigItem& it = items[i];
    TyEmit te; te.referenced = &referenced; te.local_types = &visible;
    te.self_mod = self_name; te.self_stamp = self_stamp;
    te.local_modtypes = &visible_mt; te.local_mods = &visible_mod;
    te.engine_types = &visible_eng;
    te.mods_by_name = &modscope.by_name; te.mod_members = &modscope.members;
    if (it.sigwide) { std::swap(te.vars, sw_vars); std::swap(te.shared_nodes, sw_nodes); }
    struct SwRestore {
      TyEmit& te; bool on;
      std::unordered_map<int, o::ValPtr>& v; std::unordered_map<const Ty*, o::ValPtr>& n;
      ~SwRestore() { if (on) { std::swap(te.vars, v); std::swap(te.shared_nodes, n); } }
    } sw_restore{te, it.sigwide, sw_vars, sw_nodes};
    const std::string dkey = decl_key(it.name, it.loc, it.uid);  // S574
    auto ident = ident_val(0, it.name, item_stamp[i], 0,
                           decl_name_str(it.name, dkey));  // Ident.Local
    if (it.k == SigItem::Value) {
      o::ValPtr valkind;
      if (it.prim.empty() && it.prim_native.empty() && !it.prim_external) {
        // Only a value that isn't a declared external is Val_reg: even
        // `external f : t = ""` (both names empty) stays Val_prim.
        valkind = o::vint(0);  // Val_reg
      } else {
        // Val_prim(Primitive.description): an external; inlined by consumers and
        // taking no module field.  prim_native_repr_args length must = arity.
        int arity = 0;
        for (TyPtr a = it.ty; a && a->k == Ty::Arrow; a = a->args[1]) ++arity;
        auto repr_val = [](int c) -> o::ValPtr {
          if (c >= 3 && c <= 5 && !reprshare_off()) {
            o::ValPtr &r = g_share.reprs[c];
            if (!r) r = o::vblock(0, {o::vint(c == 3 ? 1 : c == 4 ? 2 : 0)});
            return r;
          }
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
                                  prim_native_str(it.prim_native),
                                  reprs.empty() ? o::vint(0) : o::vlist(reprs),
                                  repr_val(it.prim_repr_res)});
        valkind = o::vblock(0, {desc});  // Val_prim
      }
      auto vdesc = o::vblock(0, {te.emit(it.ty), valkind, emit_loc(it.loc, dkey),
                                 it.attrs && !attr_off() ? emit_attrs(*it.attrs)
                                                         : o::vint(0) /*[] attrs*/,
                                 emit_uid(it.uid, dkey)});
      sig.push_back(o::vblock(0, {ident, vdesc, o::vint(0) /*Exported*/}));  // Sig_value
    } else if (it.k == SigItem::Module) {
      // Sig_module(id, Mp_present, module_declaration, rec_status, visibility).
      // A submodule takes a runtime field, so it must appear in the signature
      // to keep the surrounding value field layout aligned.
      o::ValPtr mty;
      int presence = 0;  // Mp_present: takes a runtime field
      if (it.is_functor) {
        // Mty_functor for `module Make (Ord : _) : S`: the functor takes a
        // runtime field (see emit_functor_mty for the parameter-chain encoding).
        mty = emit_functor_mty(it);
      } else if (!it.alias.empty()) {
        // `module name = <target>`: Mty_alias(path), Mp_absent -- an alias is
        // transparent and takes NO runtime field.  A single-component target is
        // Pident(Global unit); a dotted target (`Uid = Shape.Uid`) is
        // Pdot(Pident(Global head), comp..).  Record the HEAD unit as imported.
        size_t dot = it.alias.find('.');
        std::string head = dot == std::string::npos ? it.alias : it.alias.substr(0, dot);
        o::ValPtr path;
        if (auto lm = visible_mod.find(head); lm != visible_mod.end()) {
          // A LOCAL sibling head (`include M` strengthened `module Set =
          // M.Set`): Pident(Local{M, stamp}), not a unit global.
          path = pident_local(head, lm->second);
        } else if (stdlib_alias_module(head) && !stdalias_off()) {
          // A stdlib member (`module M = Gc.Memprof`) resolves through the
          // initial `open Stdlib`: `IdTbl.find_name` gives `Pdot (root,
          // name)` over the open's root and the looked-up string (S580).
          referenced.emplace(head, false);
          o::ValPtr root;
          if (no_share()) {
            root = o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})});
          } else {
            auto& v = g_share.heads["open:Stdlib"];
            if (!v) v = o::vblock(0, {o::vblock(2, {o::vstr("Stdlib")})});
            root = v;
          }
          path = o::vblock(1, {root, o::vstr(head)});  // Pdot(Stdlib, head)
        } else {
          referenced.emplace(head, false);
          path = o::vblock(0, {o::vblock(2, {o::vstr(head)})});  // Pident(Global head)
        }
        for (size_t pos = dot; pos != std::string::npos;) {
          size_t nd = it.alias.find('.', pos + 1);
          std::string comp = it.alias.substr(pos + 1,
              nd == std::string::npos ? std::string::npos : nd - pos - 1);
          path = o::vblock(1, {path, o::vstr(comp)});  // Pdot(path, comp)
          pos = nd;
        }
        mty = o::vblock(3, {path});  // Mty_alias
        // Mp_absent -- UNLESS the alias came from strengthening an `include`,
        // where Mtype.strengthen keeps the member's own presence and the
        // including structure really does build the field.  Writing it absent
        // made our .cmi disagree with our own .cmo: a consumer's coercion for
        // a functor argument skipped the field and read the submodule instead
        // (testsuite/tests/basic-modules/main.ml printed a pointer for 1).
        // NOINCALIASPRESENT reverts.
        presence = (it.alias_present &&
                    !cppcaml::dbg_env("NOINCALIASPRESENT")) ? 0 : 1;
      } else {
        // `module MD5 : S` (a NAMED modtype) emits Mty_ident(S) like ocamlc;
        // the inlined signature is the fallback.
        if (!it.modtype_ref.empty())
          if (o::ValPtr mp = modtype_path(it.modtype_ref)) mty = o::vblock(0, {mp});  // Mty_ident
        // A `module rec` binding's own name IS in scope inside its
        // declaration (`B.t` in B's signature is the local B), so no self
        // stamp is masked there (S560).
        const int self = it.rec_status && !cppcaml::dbg_env("NORECSTAMP")
                             ? 0 : item_stamp[i];
        if (!mty)
          mty = o::vblock(1, {o::vlist(emit_sig_items(it.sub, referenced, stamp, &visible, &visible_mt, &visible_mod, &visible_eng, &modscope, &scopes, it.name, self, item_stamp[i]))});  // Mty_signature
      }
      auto md = o::vblock(0, {mty, emit_attrs(it.attrs), emit_loc(it.loc, dkey),
                              emit_uid(it.uid, dkey)});  // module_declaration
      sig.push_back(o::vblock(3, {ident, o::vint(presence), md,
                                  o::vint(it.rec_status) /*Trec_*/,
                                  o::vint(0) /*Exported*/}));  // Sig_module
    } else if (it.k == SigItem::Modtype) {
      // Sig_modtype(id, modtype_declaration, vis).  mtd_type = Some(Mty_signature
      // sig), Some(Mty_ident) for an ALIAS `module type S2 = S1` / `= M.T`, or
      // None for an ABSTRACT `module type S`.  Takes NO runtime field, so it
      // never shifts the value layout.
      o::ValPtr mto;
      if (it.modtype_abstract)
        mto = o::vint(0);  // None
      else if (it.is_functor)
        // `module type S1 = (S0 -> S0') -> S0`: the body is a functor type, not
        // a signature -- mtd_type = Some(Mty_functor(..)) (shape_size_blowup).
        mto = o::vblock(0, {emit_functor_mty(it)});  // Some(Mty_functor)
      else if (!it.modtype_ref.empty()) {
        if (o::ValPtr mp = modtype_path(it.modtype_ref))
          mto = o::vblock(0, {o::vblock(0, {mp})});  // Some(Mty_ident)
      }
      if (!mto)
        mto = o::vblock(0, {o::vblock(1, {o::vlist(emit_sig_items(it.sub, referenced, stamp, &visible, &visible_mt, &visible_mod, &visible_eng, &modscope, &scopes))})});  // Some(Mty_signature)
      auto mtd = o::vblock(0, {mto, emit_attrs(it.attrs),
                               emit_loc(it.loc, dkey),
                               emit_uid(it.uid, dkey)});  // modtype_declaration
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
      // A Text_next reuses its group's param nodes (the path's Pdot blocks are
      // Subst's, fresh per citation, over the one lookup's strings: ext_prov).
      bool group_next = it.text_kind == 1 && !ext_group_params.empty() && !prov_off_();
      if (!it.ext_path.empty()) {
        path = te.type_path(it.ext_path, it.ext_prov);
        if (path) { eparams = &it.ext_params; status = it.text_kind; }
        // an unplaceable extended type degrades to a plain exception (valid)
      }
      if (!group_next) ext_group_params.clear();
      if (!path)
        path = predef_exn_path();  // Pident(Predef exn)
      o::ValPtr cargs;
      if (!it.ctors.empty() && !it.ctors[0].inline_record.empty()) {
        // Cstr_record inline-record payload (`exception E of {l;..}`): emit the
        // label_declarations so a consumer matching `M.E {l = ..}` resolves the
        // labels (without this, ext_match bails and the whole match collapses).
        int lstamp = 290;
        std::vector<o::ValPtr> lds;
        for (auto& l : it.ctors[0].inline_record) {
          const std::string lkey = decl_key(l.name, l.loc, l.uid);  // S574
          auto lid = decl_ident(l.name, l.stamp ? l.stamp : lstamp++, lkey);  // ld_id
          lds.push_back(o::vblock(0, {lid, o::vint(l.mut ? 1 : 0) /*ld_mutable*/,
                                      o::vint(l.atomic ? 1 : 0) /*ld_atomic*/, te.emit(l.ty),
                                      loc_none(), emit_attrs(l.attrs),
                                      emit_uid(l.uid, lkey)}));
        }
        cargs = o::vblock(1, {o::vlist(lds)});  // Cstr_record
      } else {
        std::vector<o::ValPtr> args;
        if (!it.ctors.empty()) for (auto& a : it.ctors[0].args) args.push_back(te.emit(a));
        cargs = o::vblock(0, {args.empty() ? o::vint(0) : o::vlist(args)});  // Cstr_tuple
      }
      std::vector<o::ValPtr> tparams;
      if (eparams && group_next && ext_group_params.size() == eparams->size())
        tparams = ext_group_params;
      else if (eparams) {
        for (const std::string& pn : *eparams)  // Tvar(Some source-name), e.g. "_"
          tparams.push_back(te.texpr(o::vblock(0, {o::vblock(0, {o::vstr(pn)})})));
        ext_group_params = tparams;
      }
      auto ret = it.ext_ret ? o::vblock(0, {te.emit(it.ext_ret)}) : o::vint(0);  // Some/None
      auto extcon = o::vblock(0, {path,
                                  tparams.empty() ? o::vint(0) : o::vlist(tparams),  // ext_type_params
                                  cargs, ret,
                                  o::vint(it.type_private ? 0 : 1) /*ext_private*/,
                                  loc_none(), emit_attrs(it.attrs),
                                  emit_uid(it.uid, dkey)});
      sig.push_back(o::vblock(2, {ident, extcon, o::vint(status),
                                  o::vint(0) /*Exported*/}));  // Sig_typext
    } else if (it.k == SigItem::Class) {
      // Sig_class(id, class_declaration, Trec_first, Exported), followed by
      // its two GHOST companions Sig_class_type + Sig_type (same name; the
      // reader's Signature_group asserts they follow a class, the printer
      // never shows them).  The printer reads csig_vars/csig_meths (Map.fold
      // order = alphabetical); csig_self only matters for self-aliased
      // classes, which we don't emit.
      int s_class = item_stamp[i];
      // The class's rec_status flows to its ghosts too (`class a .. and b`:
      // a's three items are all Trec_first, b's all Trec_next).
      int rs = it.rec_status ? it.rec_status : 1;
      // A `class type` decl has no Sig_class item: its idents are
      // (class_type, ghost type) at s_class / s_class+1.
      int s_clty = it.class_is_type ? s_class : s_class + 1;
      int s_ty = it.class_is_type ? s_class + 1 : s_class + 2;
      // A class item is what typeclass.ml's class_infos saves, in the shape
      // Subst's ONE copy scope leaves it (S554, hook NOCLSITEM=1):
      //   * the three (two) idents carry ONE name string, and the three
      //     declarations ONE Location (`cl.pci_loc`) and ONE uid;
      //   * a method's FIELD type P = Tpoly(ty, []) is shared by the self type,
      //     the hash type's manifest and the closed object (its nodes never
      //     reach the row variable, so limited_generalize leaves them
      //     non-generic and every instance keeps them); the csig_meths entry
      //     is M = Tpoly(spine, []) -- generalize_class_signature_spine's
      //     copy_spine of P between the two passes: an ANNOTATED method's
      //     Tarrow/Tconstr/Ttuple/Tpoly spine is copied, an inferred one's
      //     body was still a variable then, so the whole body is shared;
      //   * csig_self = Tobject(fields in SOURCE order -> rv, ref (Some
      //     (Pident <ghost type>, rv :: params))) (Ctype.set_object_name);
      //     the hash type's manifest is the same shape over a fresh row
      //     variable (instance_parameterized_type); the params are the
      //     class's own nodes (non-generic, shared everywhere);
      //   * cty_new's result and the ghost type's manifest are ONE closed
      //     object, its fields SORTED by name (unify_fields' flatten_fields
      //     rebuilt obj_ty's row) and its name ref None;
      //   * a private method has no field (Subst drops the Fabsent one) and
      //     its map entry's privacy is Mprivate (FKvar {field_kind=FKabsent}).
      const bool clsitem_off = clsitem_off_();
      o::ValPtr cloc = clsitem_off ? loc_none() : emit_loc(it.loc);
      auto cuid_ = [&] { return emit_uid(it.uid); };  // the hook: a block per use
      o::ValPtr cuid = cuid_();
      // ocamlc's idents are `Ident.create_local cl.pci_name.txt` thrice: one
      // string.  Reuse the one an earlier citation (a forward `new b`) built.
      o::ValPtr cname;
      for (int s : {s_class, s_clty, s_ty}) {
        auto f = g_share.ids.find(ident_key(0, it.name, s));
        if (f != g_share.ids.end() && f->second) { cname = f->second->fields[0]; break; }
      }
      if (!cname || clsitem_off) cname = o::vstr(it.name);
      auto cident = [&](int s) {
        if (no_share() || clsitem_off) return ident_val(0, it.name, s);
        auto& v = g_share.ids[ident_key(0, it.name, s)];
        if (!v) v = o::vblock(0, {cname, o::vint(s)});
        return v;
      };
      ident = cident(s_class);
      auto clty_ident = cident(s_clty);
      auto ty_ident = cident(s_ty);
      // Method / instance-variable label strings: one object per label,
      // cited by every Tfield and map key that names it (pcf_name.txt).
      std::map<std::string, o::ValPtr> lstr;
      auto lblstr = [&](const std::string& k) {
        if (clsitem_off) return o::vstr(k.substr(2));
        auto& v = lstr[k]; if (!v) v = o::vstr(k.substr(2)); return v;
      };
      // (mutable/privacy, virtual, ty) String.Map as a balanced marshal tree:
      // Node{l; v; d; r; h} (block tag 0), Empty = int 0.
      // Each map's (_, _, ty) triples are its own (Meths.map allocates).  The
      // tree is what Map.add built in add_method / add_instance_variable
      // order -- the fields' SOURCE order -- with Map's AVL `bal` (S554); a
      // balanced build from the sorted keys put `method a .. method b`'s b
      // at the root where ocamlc has a.  Node{l; v; d; r; h} (tag 0), Empty 0.
      struct MapEnt { std::string name; o::ValPtr key, a, b, ty; };
      struct MapNode { std::string name; o::ValPtr key, d; std::shared_ptr<MapNode> l, r; int h; };
      auto build_map = [&](std::vector<MapEnt> es) -> o::ValPtr {
        if (clsitem_off) {
          std::sort(es.begin(), es.end(),
                    [](const MapEnt& a, const MapEnt& b) { return a.name < b.name; });
          std::function<std::pair<o::ValPtr, int>(std::size_t, std::size_t)> go =
              [&](std::size_t lo, std::size_t hi) -> std::pair<o::ValPtr, int> {
            if (lo >= hi) return {o::vint(0), 0};
            std::size_t mid = lo + (hi - lo) / 2;
            auto [l, hl] = go(lo, mid);
            auto [r, hr] = go(mid + 1, hi);
            int h = 1 + std::max(hl, hr);
            o::ValPtr d = o::vblock(0, {es[mid].a, es[mid].b, es[mid].ty});
            return {o::vblock(0, {l, es[mid].key, d, r, o::vint(h)}), h};
          };
          return go(0, es.size()).first;
        }
        using T = std::shared_ptr<MapNode>;
        auto height = [](const T& t) { return t ? t->h : 0; };
        auto create = [&](const T& l, const std::string& x, o::ValPtr key, o::ValPtr d, const T& r) {
          int hl = height(l), hr = height(r);
          return std::make_shared<MapNode>(MapNode{x, key, d, l, r, hl >= hr ? hl + 1 : hr + 1});
        };
        auto bal = [&](const T& l, const std::string& x, o::ValPtr key, o::ValPtr d, const T& r) -> T {
          int hl = height(l), hr = height(r);
          if (hl > hr + 2) {
            if (height(l->l) >= height(l->r))
              return create(l->l, l->name, l->key, l->d, create(l->r, x, key, d, r));
            const T& lr = l->r;
            return create(create(l->l, l->name, l->key, l->d, lr->l), lr->name, lr->key, lr->d,
                          create(lr->r, x, key, d, r));
          }
          if (hr > hl + 2) {
            if (height(r->r) >= height(r->l))
              return create(create(l, x, key, d, r->l), r->name, r->key, r->d, r->r);
            const T& rl = r->l;
            return create(create(l, x, key, d, rl->l), rl->name, rl->key, rl->d,
                          create(rl->r, r->name, r->key, r->d, r->r));
          }
          return create(l, x, key, d, r);
        };
        std::function<T(const T&, const std::string&, o::ValPtr, o::ValPtr)> add =
            [&](const T& m, const std::string& x, o::ValPtr key, o::ValPtr d) -> T {
          if (!m) return create(nullptr, x, key, d, nullptr);
          int cmp = x.compare(m->name);
          if (cmp == 0) return create(m->l, x, key, d, m->r);  // replaced: new key, new data
          if (cmp < 0) return bal(add(m->l, x, key, d), m->name, m->key, m->d, m->r);
          return bal(m->l, m->name, m->key, m->d, add(m->r, x, key, d));
        };
        T root;
        for (auto& e : es) root = add(root, e.name, e.key, o::vblock(0, {e.a, e.b, e.ty}));
        std::function<o::ValPtr(const T&)> emit = [&](const T& t) -> o::ValPtr {
          if (!t) return o::vint(0);
          return o::vblock(0, {emit(t->l), t->key, t->d, emit(t->r), o::vint(t->h)});
        };
        return emit(root);
      };
      // self type: an OPEN object over the public methods (row ends in a Tvar
      // shared with csig_self_row).  Built up front as a patched placeholder so
      // a self-returning method (`method m = {< >}`, f.self_ref) can cite the
      // SAME node -- Printtyp then aliases the self-row proxy and prints
      // `object ('a) .. method m : 'a end`.
      o::ValPtr row_var = te.texpr(var_none_desc(false));  // Tvar None
      o::ValPtr self = te.texpr(o::vint(0));  // Tobject desc patched below
      // The bridged self node (one shared Ty across the class fields) maps to
      // csig_self, so a method type citing `'self` (`unit -> 'self`) emits a
      // back-reference to THIS node -- Printtyp then aliases the self row and
      // prints `object ('a) .. method m : unit -> 'a end` (pr7293).
      if (it.class_self) te.shared_nodes[it.class_self.get()] = self;
      // copy_spine over the emitted graph, guided by what type_approx had
      // materialized (f.approx): a fresh Tarrow / Ttuple / Tconstr / Tpoly per
      // template node, everything the template calls a variable shared.  'F'
      // copies the whole spine.  A copied Tconstr takes a fresh abbrev ref.
      static const cmiw::Approx approx_var{};
      std::function<o::ValPtr(const o::ValPtr&, const cmiw::Approx&)> spine =
          [&](const o::ValPtr& t, const cmiw::Approx& a) -> o::ValPtr {
        if (clsitem_off || a.k == 'V' || a.k == 'S' || !t || t->k != o::Value::Block ||
            t->fields.size() != 4) return t;
        const o::ValPtr& d = t->fields[0];
        if (!d || d->k != o::Value::Block) return t;
        bool full = a.k == 'F';
        auto kid = [&](std::size_t i) -> const cmiw::Approx& {
          return full ? a : i < a.kids.size() ? a.kids[i] : approx_var;
        };
        switch (d->tag) {
          case 1: {                                                    // Tarrow
            if (!full && a.k != 'A') return t;
            // the domain's Tpoly wrapper is filter_arrow's: on the spine
            o::ValPtr dom = d->fields[1];
            if (dom && dom->k == o::Value::Block && dom->fields.size() == 4 &&
                dom->fields[0]->k == o::Value::Block && dom->fields[0]->tag == 8)
              dom = te.texpr(o::vblock(8, {spine(dom->fields[0]->fields[0], kid(0)),
                                           dom->fields[0]->fields[1]}));
            else dom = spine(dom, kid(0));
            return te.texpr(o::vblock(1, {d->fields[0], dom, spine(d->fields[2], kid(1)),
                                          d->fields[3]}));
          }
          case 2: {                                                    // Ttuple
            if (!full && a.k != 'T') return t;
            std::vector<o::ValPtr> es; std::size_t i = 0;
            for (o::ValPtr c = d->fields[0]; c && c->k == o::Value::Block; c = c->fields[1], ++i)
              es.push_back(o::vblock(0, {c->fields[0]->fields[0],
                                         spine(c->fields[0]->fields[1], kid(i))}));
            return te.texpr(o::vblock(2, {es.empty() ? o::vint(0) : o::vlist(es)}));
          }
          case 3: {                                                    // Tconstr
            if (!full && a.k != 'C') return t;
            std::vector<o::ValPtr> as; std::size_t i = 0;
            for (o::ValPtr c = d->fields[1]; c && c->k == o::Value::Block; c = c->fields[1], ++i)
              as.push_back(spine(c->fields[0], kid(i)));
            return te.texpr(o::vblock(3, {d->fields[0], as.empty() ? o::vint(0) : o::vlist(as),
                                          o::vblock(0, {o::vint(0)})}));
          }
          case 8:                                                      // Tpoly
            if (!full) return t;
            return te.texpr(o::vblock(8, {spine(d->fields[0], a), d->fields[1]}));
          case 9:                                                      // Tpackage
            if (!full) return t;
            return te.texpr(o::vblock(9, {d->fields[0]}));
          default: return t;
        }
      };
      // A node from which the self type is reachable is generic (an ancestor
      // of the row variable under limited_generalize), so every instance of
      // the class copies it: the closed object's and the hash type's copies
      // of a self-citing method type (`method s = self`) are fresh nodes over
      // THEIR object; everything not reaching self stays shared.
      std::unordered_map<const o::Value*, bool> reach_memo;
      std::function<bool(const o::ValPtr&)> reaches_self = [&](const o::ValPtr& t) -> bool {
        if (!t || t->k != o::Value::Block || t->fields.size() != 4) return false;
        if (t == self) return true;
        auto f = reach_memo.find(t.get());
        if (f != reach_memo.end()) return f->second;
        reach_memo[t.get()] = false;  // a back edge: not through here
        bool r = false;
        const o::ValPtr& d = t->fields[0];
        if (d && d->k == o::Value::Block) {
          auto each = [&](const o::ValPtr& l) {
            for (o::ValPtr x = l; x && x->k == o::Value::Block; x = x->fields[1])
              r = reaches_self(x->fields[0]) || r;
          };
          switch (d->tag) {
            case 1: r = reaches_self(d->fields[1]) | reaches_self(d->fields[2]); break;   // Tarrow
            case 2: for (o::ValPtr x = d->fields[0]; x && x->k == o::Value::Block; x = x->fields[1])
                      r = reaches_self(x->fields[0]->fields[1]) || r; break;             // Ttuple
            case 3: each(d->fields[1]); break;                                            // Tconstr
            case 4: r = reaches_self(d->fields[0]);                                       // Tobject
                    if (d->fields[1]->k == o::Value::Block && d->fields[1]->fields[0]->k == o::Value::Block)
                      each(d->fields[1]->fields[0]->fields[0]->fields[1]);
                    break;
            case 5: r = reaches_self(d->fields[2]) | reaches_self(d->fields[3]); break;   // Tfield
            case 8: r = reaches_self(d->fields[0]); break;                                // Tpoly
            default: break;
          }
        }
        reach_memo[t.get()] = r;
        return r;
      };
      std::function<o::ValPtr(const o::ValPtr&, const o::ValPtr&,
                              std::unordered_map<const o::Value*, o::ValPtr>&)> subst_self =
          [&](const o::ValPtr& t, const o::ValPtr& target,
              std::unordered_map<const o::Value*, o::ValPtr>& memo) -> o::ValPtr {
        if (t == self) return target;
        if (!reaches_self(t)) return t;
        auto f = memo.find(t.get());
        if (f != memo.end()) return f->second;
        o::ValPtr shell = te.texpr(o::vint(0));
        memo[t.get()] = shell;
        const o::ValPtr& d = t->fields[0];
        auto sub = [&](const o::ValPtr& x) { return subst_self(x, target, memo); };
        auto lst = [&](const o::ValPtr& l) {
          std::vector<o::ValPtr> out;
          for (o::ValPtr x = l; x && x->k == o::Value::Block; x = x->fields[1]) out.push_back(sub(x->fields[0]));
          return out.empty() ? o::vint(0) : o::vlist(out);
        };
        o::ValPtr nd;
        switch (d->tag) {
          case 1: nd = o::vblock(1, {d->fields[0], sub(d->fields[1]), sub(d->fields[2]), d->fields[3]}); break;
          case 2: {
            std::vector<o::ValPtr> es;
            for (o::ValPtr x = d->fields[0]; x && x->k == o::Value::Block; x = x->fields[1])
              es.push_back(o::vblock(0, {x->fields[0]->fields[0], sub(x->fields[0]->fields[1])}));
            nd = o::vblock(2, {es.empty() ? o::vint(0) : o::vlist(es)}); break;
          }
          case 3: nd = o::vblock(3, {d->fields[0], lst(d->fields[1]), o::vblock(0, {o::vint(0)})}); break;
          case 4: {
            o::ValPtr nm = d->fields[1];
            if (nm->k == o::Value::Block && nm->fields[0]->k == o::Value::Block) {
              const o::ValPtr& pr = nm->fields[0]->fields[0];
              nm = o::vblock(0, {o::vblock(0, {o::vblock(0, {pr->fields[0], lst(pr->fields[1])})})});
            } else nm = o::vblock(0, {o::vint(0)});
            nd = o::vblock(4, {sub(d->fields[0]), nm}); break;
          }
          case 5: nd = o::vblock(5, {d->fields[0], d->fields[1], sub(d->fields[2]), sub(d->fields[3])}); break;
          case 8: nd = o::vblock(8, {sub(d->fields[0]), d->fields[1]}); break;
          default: memo[t.get()] = t; return t;  // a row / package citing self: kept
        }
        shell->fields[0] = nd;
        return shell;
      };
      struct Meth { std::string name; o::ValPtr P; bool priv, virt; };
      std::vector<Meth> meths;             // public methods, source order
      std::vector<MapEnt> vars_m, meths_m;
      std::vector<std::string> mnames;
      std::vector<TyPtr> mtys;             // the closed object placeholder
      for (auto& f : it.class_fields) {
        if (f.is_method) {
          o::ValPtr fty = f.self_ref ? self : te.emit(f.ty);
          o::ValPtr P = te.texpr(o::vblock(8, {fty, o::vint(0)}));  // Tpoly(ty,[])
          o::ValPtr M = f.approx.k == 'S' && !clsitem_off
              ? P : te.texpr(o::vblock(8, {spine(fty, f.approx), o::vint(0)}));
          o::ValPtr priv = f.priv
              ? (clsitem_off ? o::vblock(0, {o::vint(2)})
                             : o::vblock(0, {o::vblock(0, {o::vint(2)})}))  // Mprivate(FKvar{FKabsent})
              : o::vint(0);                                                  // Mpublic
          meths_m.push_back({f.name, lblstr("m:" + f.name), priv, o::vint(f.virt ? 0 : 1), M});
          if (!f.priv) {
            meths.push_back({f.name, P, f.priv, f.virt});
            mnames.push_back(f.name); mtys.push_back(f.ty);
          }
        } else {
          vars_m.push_back({f.name, lblstr("v:" + f.name), o::vint(f.mut ? 1 : 0),
                            o::vint(f.virt ? 0 : 1), te.emit(f.ty)});
        }
      }
      // A field chain over the public methods in `order`, ending in `tail`,
      // for the object `target` (its self-citing method types re-copied).
      auto chain_of = [&](const std::vector<std::size_t>& order, o::ValPtr tail,
                          const o::ValPtr& target) {
        std::unordered_map<const o::Value*, o::ValPtr> memo;
        o::ValPtr c = tail;
        for (std::size_t k = order.size(); k-- > 0;) {
          const Meth& m = meths[order[k]];
          o::ValPtr ty = target == self || clsitem_off ? m.P : subst_self(m.P, target, memo);
          c = te.texpr(o::vblock(5, {lblstr("m:" + m.name), o::vint(1) /*FKpublic*/,
                                     ty, c}));  // Tfield
        }
        return c;
      };

      std::vector<std::size_t> src_order(meths.size()), sorted_order;
      for (std::size_t k = 0; k < meths.size(); ++k) src_order[k] = k;
      sorted_order = src_order;
      std::sort(sorted_order.begin(), sorted_order.end(), [&](std::size_t a, std::size_t b) {
        return meths[a].name < meths[b].name;
      });
      auto cpath = pident_local(it.name, s_ty);
      // cty_params: the `['a, _] c` type params -- the class's own nodes,
      // cited by every item and by the objects' names.
      std::vector<o::ValPtr> pnodes;
      for (std::size_t pi = 0; pi < it.class_params.size(); ++pi)
        // A self-typed param (`object (self : 'a)`) shares the SAME node as
        // csig_self, so Printtyp aliases it and prints `object ('a) constraint`.
        pnodes.push_back((int)pi == it.class_self_param ? self
                                                        : te.emit(it.class_params[pi]));
      // The class params as `target`'s instance sees them (a self-typed param
      // is that object).
      auto params_for = [&](const o::ValPtr& target) {
        std::vector<o::ValPtr> ps;
        for (auto& p : pnodes) ps.push_back(p == self ? target : p);
        return ps;
      };
      auto cty_params = [&] {  // a fresh list per item (List.map)
        if (pnodes.empty()) return o::vint(0);
        return o::vlist(pnodes);
      };
      // ref (Some (Pident ghost, rv :: params)) -- Ctype.set_object_name
      auto nominal = [&](const o::ValPtr& rv, const o::ValPtr& target) -> o::ValPtr {
        if (clsitem_off) return o::vblock(0, {o::vint(0)});  // ref None
        std::vector<o::ValPtr> l{rv};
        for (auto& p : params_for(target)) l.push_back(p);
        return o::vblock(0, {o::vblock(0, {o::vblock(0, {cpath, o::vlist(l)})})});
      };
      self->fields[0] = o::vblock(4, {chain_of(src_order, row_var, self),
                                      nominal(row_var, self)});  // Tobject
      auto csig = [&] {  // a fresh record and fresh maps per item
        return o::vblock(0, {self, row_var, o::vint(2) /*dummy FKabsent*/,
                             build_map(vars_m), build_map(meths_m)});
      };
      o::ValPtr cty = o::vblock(1, {csig()});  // Cty_signature (Sig_class)
      o::ValPtr body_cty = clsitem_off ? cty : o::vblock(1, {csig()});  // Btype.class_body (Sig_class_type)
      // An ALIAS class (`class c = with_param args`) or a NAMED class-type
      // annotation (`class b : B.a = object..end`): the stored class type
      // is Cty_constr(target, [], inner) -- Printtyp prints `class c :
      // with_param` from the path.
      if (!it.class_constr_ref.empty()) {
        o::ValPtr cp;
        if (it.class_constr_ref.find('.') == std::string::npos) {
          if (auto lc = local_classes.find(it.class_constr_ref);
              lc != local_classes.end())
            cp = pident_local(it.class_constr_ref, lc->second);
        } else {
          cp = te.type_path(it.class_constr_ref);  // local-mod / global Pdot chain
        }
        if (cp) {
          cty = o::vblock(0, {cp, o::vint(0) /*[]*/, cty});  // Cty_constr
          body_cty = clsitem_off ? cty : o::vblock(0, {cp, o::vint(0), body_cty});
        }
      }
      for (std::size_t p = it.class_arrow_doms.size(); p-- > 0;) {
        int lk = p < it.class_arrow_lks.size() ? it.class_arrow_lks[p] : 0;
        const std::string& lb = p < it.class_arrow_lbls.size() ? it.class_arrow_lbls[p]
                                                               : it.name /*unused*/;
        o::ValPtr lbl = lk == 1 ? o::vblock(0, {o::vstr(lb)})
                      : lk == 2 ? o::vblock(1, {o::vstr(lb)})
                                : o::vint(0);
        TyPtr dom = it.class_arrow_doms[p];
        if (lk == 2 && !(dom->k == Ty::Constr && dom->name == "option"))
          dom = ty_constr("option", {dom});  // optional param's stored domain
        cty = o::vblock(2, {lbl, te.emit(dom), cty});  // Cty_arrow
        if (clsitem_off) body_cty = cty;
      }
      // The CLOSED object of the public methods (sorted, row Tnil, unnamed):
      // cty_new's result and the ghost type's manifest, one node.
      TyPtr closed_ty = ty_object(mnames, mtys);  // placeholder Ty for the arrows
      o::ValPtr closed;
      if (clsitem_off) {
        closed = te.emit(closed_ty);
      } else {
        closed = te.texpr(o::vint(0));
        closed->fields[0] = o::vblock(4, {chain_of(sorted_order, te.texpr(o::vint(0)) /*Tnil*/, closed),
                                          o::vblock(0, {o::vint(0)})});  // Tobject(.., ref None)
      }
      te.shared_nodes[closed_ty.get()] = closed;
      // cty_new: None for a virtual class; else the constructor's value type
      // params -> <closed object of public methods>
      o::ValPtr cnew;
      if (it.class_virtual) {
        cnew = o::vint(0);
      } else if (!it.class_constr_ref.empty()) {
        // alias class: ocamlc stores cty_new = Tconstr(target's ghost type)
        cnew = o::vblock(0, {te.emit(ty_constr(it.class_constr_ref, {}))});
      } else {
        TyPtr nt = closed_ty;
        for (std::size_t p = it.class_arrow_doms.size(); p-- > 0;)
          nt = ty_arrow_lbl(it.class_arrow_doms[p], nt,
                            p < it.class_arrow_lks.size() ? it.class_arrow_lks[p] : 0,
                            p < it.class_arrow_lbls.size() ? it.class_arrow_lbls[p] : "");
        cnew = o::vblock(0, {te.emit(nt)});
      }
      te.shared_nodes.erase(closed_ty.get());  // the placeholder's address may be reused
      // The class's variance signature (Typedecl_variance.update_class_decls,
      // computed by the checker), Variance.unknown (7) per param where it is
      // not.  ONE list, cited by the four declarations.
      o::ValPtr variance = [&] {
        if (it.class_params.empty()) return o::vint(0);
        std::vector<o::ValPtr> v;
        for (std::size_t pi = 0; pi < it.class_params.size(); ++pi)
          v.push_back(o::vint(!clsitem_off && it.class_variances.size() == it.class_params.size()
                                  ? it.class_variances[pi] : 7));
        return o::vlist(v);
      }();
      auto cty_variance = [&]() -> o::ValPtr {
        if (it.class_params.empty() || !clsitem_off) return variance;
        std::vector<o::ValPtr> v(it.class_params.size(), o::vint(7));  // a list per use
        return o::vlist(v);
      };
      if (!it.class_is_type) {
        auto cdecl = o::vblock(0, {cty_params(), cty, cpath, cnew,
                                   cty_variance(), cloc,
                                   o::vint(0) /*attrs*/, clsitem_off ? cuid_() : cuid});
        sig.push_back(o::vblock(5, {ident, cdecl, o::vint(rs),
                                    o::vint(0) /*Exported*/}));  // Sig_class
      }
      // The hash type (`#c`) and the ghost type (`c`): abstract declarations
      // over the class's params whose manifests are the open and the closed
      // object; the class's loc and uid are theirs too (typeclass.ml `cl_td`)
      auto mk_tdecl = [&](o::ValPtr man, const o::ValPtr& target) {
        std::size_t n = clsitem_off ? 0 : it.class_params.size();
        std::vector<o::ValPtr> sep(n, o::vint(2) /*Deepsep*/);
        return o::vblock(0, {n ? o::vlist(params_for(target)) : o::vint(0), o::vint((long long)n),
                             o::vblock(0, {o::vint(0)}) /*Type_abstract Definition*/,
                             o::vint(1) /*Public*/, man,
                             n ? cty_variance() : o::vint(0),
                             n ? o::vlist(sep) : o::vint(0),
                             o::vint(0), o::vint(0),
                             cloc, o::vint(0), o::vint(0), o::vint(0),
                             clsitem_off ? cuid_() : cuid});
      };
      o::ValPtr hash_man = o::vint(0);  // None
      o::ValPtr hself = self;
      if (!clsitem_off) {
        o::ValPtr hrow = te.texpr(var_none_desc(false));
        hself = te.texpr(o::vint(0));
        hself->fields[0] = o::vblock(4, {chain_of(src_order, hrow, hself), nominal(hrow, hself)});
        hash_man = o::vblock(0, {hself});
      }
      // Sig_class_type: a ghost after a class, the REAL item for `class type`
      auto clty = o::vblock(0, {cty_params(), body_cty, cpath,
                                mk_tdecl(hash_man, hself), cty_variance(),
                                cloc, o::vint(0), clsitem_off ? cuid_() : cuid});
      sig.push_back(o::vblock(6, {clty_ident, clty, o::vint(rs), o::vint(0)}));  // Sig_class_type
      // ghost Sig_type c = <closed public object> (what `val o : c` cites)
      auto g_tdecl = mk_tdecl(o::vblock(0, {closed}), closed);
      sig.push_back(o::vblock(1, {ty_ident, g_tdecl, o::vint(rs), o::vint(0)}));  // Sig_type
    } else {
      // type_declaration (14 fields).  Type_abstract kind; a manifest makes it an
      // alias (`type t = manifest`).  Variant/record kinds: the climb.
      std::vector<o::ValPtr> ps;
      // Decl-order scoping (see type_vis_end above): this decl's own citation
      // maps exclude later siblings / a nonrec self so they resolve outward.
      if (type_vis_end[i] != items.size()) {
        te.local_types = ordered_types(type_vis_end[i]);
        te.engine_types = ordered_eng(type_vis_end[i]);
      }
      // A `with type`-spliced decl: its params+manifest were written
      // with_scope_skip levels up, so bare names there resolve against THAT
      // scope's types (no capture by the refined sig's own decls).  The kind
      // (ctors/labels, from the base modtype body) still resolves here.
      const std::unordered_map<std::string, int>* cur_types = te.local_types;
      if (it.with_scope_skip > 0 &&
          scopes.size() > (std::size_t)it.with_scope_skip)
        te.local_types = scopes[scopes.size() - 1 - it.with_scope_skip];
      for (auto& p : it.params) ps.push_back(te.emit(p));
      auto man = it.manifest ? o::vblock(0, {te.emit(it.manifest)}) : o::vint(0);  // Some/None
      te.local_types = cur_types;
      o::ValPtr kind;
      if (!it.ctors.empty()) {
        int cstamp = 270;
        std::vector<o::ValPtr> cds;
        for (auto& c : it.ctors) {
          const std::string ckey = decl_key(c.name, c.loc, c.uid);  // S574
          auto cid = decl_ident(c.name, c.stamp ? c.stamp : cstamp++, ckey);  // cd_id
          o::ValPtr cargs;
          if (!c.inline_record.empty()) {  // Cstr_record of label_declaration list
            int lstamp = 290;
            std::vector<o::ValPtr> lds;
            for (auto& l : c.inline_record) {
              const std::string lkey = decl_key(l.name, l.loc, l.uid);  // S574
              auto lid = decl_ident(l.name, l.stamp ? l.stamp : lstamp++, lkey);  // ld_id
              lds.push_back(o::vblock(0, {lid, o::vint(l.mut ? 1 : 0) /*ld_mutable*/,
                                          o::vint(l.atomic ? 1 : 0) /*ld_atomic*/, te.emit(l.ty),
                                          emit_loc(l.loc, lkey), emit_attrs(l.attrs),
                                          emit_uid(l.uid, lkey)}));
            }
            cargs = o::vblock(1, {o::vlist(lds)});  // Cstr_record
          } else {
            std::vector<o::ValPtr> args;
            for (auto& a : c.args) args.push_back(te.emit(a));
            cargs = o::vblock(0, {args.empty() ? o::vint(0) : o::vlist(args)});  // Cstr_tuple
          }
          auto cres = c.res ? o::vblock(0, {te.emit(c.res)}) : o::vint(0);  // cd_res Some/None
          cds.push_back(o::vblock(0, {cid, cargs, cres, emit_loc(c.loc, ckey),
                                      emit_attrs(c.attrs), emit_uid(c.uid, ckey)}));
        }
        // variant_representation: Variant_regular (0) or Variant_unboxed (1),
        // the latter for a single single-field ctor marked `[@@unboxed]`.
        kind = o::vblock(2, {o::vlist(cds), o::vint(it.type_unboxed ? 1 : 0)});  // Type_variant
      } else if (!it.labels.empty()) {
        int lstamp = 280;
        std::vector<o::ValPtr> lds;
        for (auto& l : it.labels) {
          const std::string lkey = decl_key(l.name, l.loc, l.uid);  // S574
          auto lid = decl_ident(l.name, l.stamp ? l.stamp : lstamp++, lkey);  // ld_id
          lds.push_back(o::vblock(0, {lid, o::vint(l.mut ? 1 : 0) /*ld_mutable*/,
                                      o::vint(l.atomic ? 1 : 0) /*ld_atomic*/, te.emit(l.ty),
                                      emit_loc(l.loc, lkey), emit_attrs(l.attrs),
                                      emit_uid(l.uid, lkey)}));
        }
        // record_representation: Record_regular (const 0), Record_float
        // (const 1: every field a float, S561) or, for a single-field
        // `[@@unboxed]` record, Record_unboxed of bool (block tag 0; false = not
        // an inlined record).
        auto rep = it.type_unboxed ? o::vblock(0, {o::vint(0)})
                 : o::vint(it.type_record_float && !unboxdef_off() ? 1 : 0);
        kind = o::vblock(1, {o::vlist(lds), rep});  // Type_record
      } else if (it.type_open) {
        kind = o::vint(0);  // Type_open (`type t = ..`), the lone constant ctor
      } else if (it.type_empty_variant) {
        kind = o::vblock(2, {o::vint(0), o::vint(0)});  // Type_variant([], regular)
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
          [&] {
            std::vector<o::ValPtr> v;
            for (std::size_t i = 0; i < it.params.size(); ++i)
              v.push_back(o::vint(i < it.type_variances.size()
                                      ? it.type_variances[i] : 7));
            return v.empty() ? o::vint(0) : o::vlist(v);
          }(),
          // type_separability: Ind unless computed/carried (S576).
          [&] {
            std::vector<o::ValPtr> v;
            for (std::size_t i = 0; i < it.params.size(); ++i)
              v.push_back(o::vint(!sep_off() && i < it.type_separability.size()
                                      ? it.type_separability[i] : 0));
            return v.empty() ? o::vint(0) : o::vlist(v);
          }(),
          o::vint(0), o::vint(0),                      // is_newtype false, expansion_scope 0
          emit_loc(it.loc, dkey),                      // type_loc
          // type_attributes: Printtyp derives the printed `[@@immediate]` /
          // `[@@immediate64]` from Type_immediacy.of_attributes of THIS field (not
          // from type_immediate), so emit the real attribute when WRITTEN in
          // source -- not for a derived all-const-variant Always.
          [&]() -> o::ValPtr {
            if (it.attrs && !attr_off()) return emit_attrs(*it.attrs);  // S577
            if (!it.type_immediate_attr) return o::vint(0);  // []
            const char* nm = it.type_immediate_attr == 2 ? "immediate64" : "immediate";
            auto attr = o::vblock(0, {
                o::vblock(0, {o::vstr(nm), loc_none()}),  // attr_name : string loc
                o::vblock(0, {o::vint(0)}),               // attr_payload = PStr []
                loc_none()});                             // attr_loc
            return o::vlist({attr});
          }(),
          o::vint(it.type_immediate),                  // type_immediate
          // type_unboxed_default: true for an unboxable declaration written
          // without `[@@unboxed]`/`[@@boxed]` (typedecl's `unboxed_default`).
          o::vint(it.type_unboxed_default && !unboxdef_off() ? 1 : 0),
          emit_uid(it.uid, dkey)});                    // type_uid
      sig.push_back(o::vblock(1, {ident, tdecl,
                                  // Trec_first, or Trec_next for the `and`
                                  // members of a mutually-recursive group;
                                  // -1 = an explicit `type nonrec` (Trec_not)
                                  o::vint(it.rec_status < 0
                                              ? 0
                                              : it.rec_status ? it.rec_status : 1),
                                  o::vint(0) /*Exported*/}));
    }
  }
  return sig;
}

// Read a .cmi's WHOLE crcs list: (interface name, crc) for the module itself and
// every interface it imports.  crc is empty for a `None` entry (module alias, no
// digest).  Used to populate a .cmo's cu_imports so the linker can reject units
// built against inconsistent interfaces.  Empty vector on any failure.
std::vector<std::pair<std::string, std::string>> read_cmi_crcs(const std::string& path) {
  std::vector<std::pair<std::string, std::string>> r;
  std::ifstream in(path, std::ios::binary);
  if (!in) return r;
  std::vector<std::uint8_t> bytes = slurp_bytes(in);
  std::size_t off = 0;
  if (!find_marshal_magic(bytes, off)) return r;
  try {
    m::Arena arena;
    // Only the crc table (2nd Marshal value) is needed: skip the signature by its
    // declared length instead of decoding its whole node graph.
    m::skip_value(bytes.data(), bytes.size(), off);
    if (!find_marshal_magic(bytes, off)) return r;
    std::size_t cur = m::read_value(bytes.data(), bytes.size(), off, arena);  // crc list
    arena.finalize();
    while (arena[cur].kind == m::Value::Kind::Block && arena[cur].fields.size() == 2) {
      const m::Value& entry = arena[arena[cur].fields[0]];          // (name, crc option)
      if (entry.kind == m::Value::Kind::Block && entry.fields.size() >= 2) {
        std::string name = arena[entry.fields[0]].str(), crc;
        const m::Value& crcopt = arena[entry.fields[1]];
        if (crcopt.kind == m::Value::Kind::Block && !crcopt.fields.empty())
          crc = arena[crcopt.fields[0]].str();                        // Some digest
        r.emplace_back(std::move(name), std::move(crc));
      }
      cur = arena[cur].fields[1];                                   // list tail
    }
  } catch (const std::exception&) {
    return {};
  }
  return r;
}

// The interface CRC of a compilation-unit global (`A`, `Lexer`, `Stdlib__List`):
// locate its .cmi on the include path and read its self-CRC.  Empty if not found.
std::string module_cmi_crc(const std::string& mod) {
  return read_cmi_self_crc(resolve_cmi_global(mod));
}

// Assign Shape.Uid to every genuinely-new declaration in the current unit,
// mirroring the typer's traversal so the ids match ocamlc's byte-for-byte:
// signature items in document order; a `type .. and ..` recursion group (a head
// with rec_status 0/1/-1 followed by rec_status==2 continuations) is TWO-PASS --
// all its type_uids first, then all its ctor/label uids (an inline-record ctor's
// labels before its own cd_uid); a submodule's contents get uids before the
// module's own (post-order).  `c` is the per-unit counter (shared, starts 0).
// NOTE: does not yet PRESERVE uids for include/functor/alias members (they carry
// their source unit's uid) -- correct for self-contained modules; see the
// byte-identity plan.
// A declaration this unit COPIED from a dependency's .cmi already carries the
// uid its own unit gave it (S564): it is not the writer's to number and it
// costs the counter nothing, since `Subst` mints nothing when it renames the
// idents of a signature an `include` / a functor application re-exports.
static bool foreign_uid(const cmiw::Uid& u, const std::string& unit) {
  return u.k == cmiw::Uid::Item && !u.unit.empty() && u.unit != unit;
}
static bool mtfall_off() {
  static const bool off = cppcaml::dbg_env("NOUIDMTFALL") != nullptr ||
                          cppcaml::dbg_env("NOSHARE573") != nullptr;
  return off;
}
static void assign_uids(std::vector<SigItem>& items, const std::string& unit,
                        bool intf, int& c) {
  auto mk = [&]() { cmiw::Uid u; u.k = cmiw::Uid::Item; u.unit = unit;
                    u.id = c++; u.intf = intf; return u; };
  auto own = [&](cmiw::Uid& u) { if (!foreign_uid(u, unit)) u = mk(); };
  for (std::size_t i = 0; i < items.size();) {
    SigItem& it = items[i];
    if (it.k == SigItem::Type) {
      std::size_t j = i + 1;  // gather the recursion group [i, j)
      while (j < items.size() && items[j].k == SigItem::Type &&
             items[j].rec_status == 2) ++j;
      bool copied = false;
      for (std::size_t k = i; k < j; ++k) {                     // pass 1: type_uids
        copied = copied || foreign_uid(items[k].uid, unit);
        own(items[k].uid);
      }
      int inline_ctors = 0;
      for (std::size_t k = i; k < j; ++k) {                     // pass 2: ctors/labels
        for (auto& ct : items[k].ctors) {
          for (auto& l : ct.inline_record) own(l.uid);  // inline labels before cd
          own(ct.uid);
          if (!ct.inline_record.empty() && !copied) ++inline_ctors;
        }
        for (auto& l : items[k].labels) own(l.uid);
      }
      // Pass 3: each inline-record constructor made datarepr build a HIDDEN
      // record type_declaration (typing/datarepr.ml ~94), burning 5 uids that
      // never reach the signature -- advance the counter so the NEXT visible
      // decl's id matches ocamlc (measured constant: 5 per inline-record ctor,
      // independent of field count; e.g. outcometree's out_type group = 2 -> 10).
      c += 5 * inline_ctors;
      i = j;
    } else if (it.k == SigItem::Value) {
      own(it.uid); ++i;
    } else if (it.k == SigItem::Exception) {
      own(it.uid);
      for (auto& ct : it.ctors) for (auto& l : ct.inline_record) own(l.uid);
      ++i;
    } else if (it.k == SigItem::Module) {
      assign_uids(it.sub, unit, intf, c);  // contents first (post-order)
      own(it.uid); ++i;
    } else if (it.k == SigItem::Modtype) {
      // `module type S = sig .. end`: the body's items are numbered like any
      // other signature's, before the module type's own uid -- the same law
      // `assign_uids_mapped` below already follows (S563).  Leaving them out
      // here left them `Uid.Internal` AND shifted every later id by the
      // body's length, so a file that lost the typing map lost far more than
      // the declarations the map was missing (S573; NOUIDMTFALL=1, alias
      // NOSHARE573=1, leaves the body alone as before).
      if (!mtfall_off()) assign_uids(it.sub, unit, intf, c);
      own(it.uid); ++i;
    } else {
      ++i;  // Class: uids not yet modelled
    }
  }
}

// THE UID TYPING GAVE EACH DECLARATION (S551).  `assign_uids` above numbers
// the saved signature's items, which is the right answer only when nothing
// LOCAL was typed on the way: a pattern variable, a `for` index, a newtype, a
// local module are all `Uid.mk` calls too.  `cppcaml::typing_uid_map` replays
// the parsetree in typing order and records the counter at each saved
// declaration; this pass hands those uids to the items.  It walks exactly the
// items `assign_uids` numbers, and returns false -- leaving the signature
// untouched -- as soon as one of them has no recorded uid, so a file either
// takes the model whole or keeps the old numbering whole.
// S563's facet revert (and the whole-slice one).
static bool mtbody_off() {
  static const bool off = cppcaml::dbg_env("NOUIDMTBODY") != nullptr ||
                          cppcaml::dbg_env("NOUID563") != nullptr;
  return off;
}

static bool assign_uids_mapped(std::vector<SigItem>& items,
                               const std::string& unit, bool intf,
                               const std::map<std::string, int>& uids,
                               const std::string& path, bool commit) {
  auto at = [&](char kind, const std::string& name, cmiw::Uid* out) {
    auto it = uids.find(uidkey(kind, path, name));
    // THE CROSS-UNIT HALF OF THE COPY LAW (S564).  A declaration re-exported
    // from ANOTHER unit (`module M = Set.Make (Int)`, `include <external>`)
    // keeps that unit's uid, which the walk cannot know and does not record:
    // the item carries it already, decoded from the dependency's .cmi.
    if (it == uids.end()) return foreign_uid(*out, unit);
    if (commit) { out->k = cmiw::Uid::Item; out->unit = unit;
                  out->id = it->second; out->intf = intf; }
    return true;
  };
  for (SigItem& it : items) {
    if (it.k == SigItem::Type) {
      if (!at('t', it.name, &it.uid)) return false;
      for (auto& ct : it.ctors) {
        for (auto& l : ct.inline_record)
          if (!at('L', it.name + "#" + ct.name + "." + l.name, &l.uid)) return false;
        if (!at('c', it.name + "#" + ct.name, &ct.uid)) return false;
      }
      for (auto& l : it.labels)
        if (!at('l', it.name + "." + l.name, &l.uid)) return false;
    } else if (it.k == SigItem::Value) {
      if (!at('v', it.name, &it.uid)) return false;
    } else if (it.k == SigItem::Exception) {
      if (!at('e', it.name, &it.uid)) return false;
      for (auto& ct : it.ctors)
        for (auto& l : ct.inline_record)
          if (!at('L', it.name + "#" + ct.name + "." + l.name, &l.uid)) return false;
    } else if (it.k == SigItem::Module) {
      const std::string sub = path.empty() ? it.name : path + "." + it.name;
      // A functor's PARAMETER signature is a module type body too (S563).
      if (it.is_functor && !mtbody_off() &&
          !assign_uids_mapped(it.param_sig, unit, intf, uids,
                              sub + ".!" + it.functor_param, commit))
        return false;
      if (!assign_uids_mapped(it.sub, unit, intf, uids, sub, commit)) return false;
      if (!at('m', it.name, &it.uid)) return false;
    } else if (it.k == SigItem::Modtype) {
      // `module type S = sig .. end`: the typer numbers the body's items
      // like any other signature's (S563; NOUIDMTBODY=1 leaves them
      // Internal, as before).
      if (!mtbody_off() &&
          !assign_uids_mapped(it.sub, unit, intf, uids,
                              (path.empty() ? std::string() : path + ".") +
                                  "%" + it.name,
                              commit))
        return false;
      if (!at('M', it.name, &it.uid)) return false;
    } else if (it.k == SigItem::Class) {
      // `class c`, `class type c` and `type c` all carry the ONE uid
      // `type_classes` minted for the class (S553).
      if (!at('C', it.name, &it.uid)) return false;
    }
  }
  return true;
}

std::vector<Import> cmi_imports(const std::set<std::string>& loaded,
                                const std::string& self, bool pervasives) {
  std::map<std::string, std::string> crc;
  std::set<std::string> units = loaded;
  if (pervasives) units.insert("Stdlib");
  for (const std::string& g : units)
    for (auto& [n, c] : read_cmi_crcs(resolve_cmi_global(g)))
      if (!c.empty() && n != self) crc[n] = c;
  std::vector<Import> out;
  for (auto it = crc.rbegin(); it != crc.rend(); ++it)
    out.push_back({it->first, it->second});
  return out;
}

// AN INFERRED SIGNATURE'S VALUE TYPES ARE NORMALIZED (S550).  Typemod's
// type_implementation runs Ctype.normalize_type over every Sig_value of the
// inferred signature of a .ml WITHOUT an .mli (normalize_modtype: into module
// bodies and a functor's RESULT; not its parameter, not a module type, not a
// class) before saving it: a Tvariant's row fields are sorted by LABEL
// (string compare, ascending) where Typetexp built them hash-descending.
// The walk covers the emitted value graph like TyIdWalk, before the ids.
struct RowNormWalk {
  std::unordered_set<const o::Value*> seen;
  static bool blk(const o::ValPtr& v) { return v && v->k == o::Value::Block; }
  static std::vector<o::ValPtr> list(o::ValPtr v) {
    std::vector<o::ValPtr> out;
    while (blk(v) && v->fields.size() == 2) { out.push_back(v->fields[0]); v = v->fields[1]; }
    return out;
  }
  void opt(const o::ValPtr& v) { if (blk(v)) ty(v->fields[0]); }
  void tys(const o::ValPtr& l) { for (auto& t : list(l)) ty(t); }
  void package(const o::ValPtr& p) {
    for (auto& c : list(p->fields[1])) ty(c->fields[1]);
  }
  void ty(const o::ValPtr& t) {
    if (!blk(t) || t->fields.size() != 4 || !seen.insert(t.get()).second) return;
    const o::ValPtr& d = t->fields[0];
    if (!blk(d)) return;
    switch (d->tag) {
      case 1: ty(d->fields[1]); ty(d->fields[2]); break;             // Tarrow
      case 2: for (auto& e : list(d->fields[0])) ty(e->fields[1]); break;  // Ttuple
      case 3: tys(d->fields[1]); break;                              // Tconstr
      case 4:                                                        // Tobject
        ty(d->fields[0]);
        if (blk(d->fields[1]) && blk(d->fields[1]->fields[0]))
          tys(d->fields[1]->fields[0]->fields[0]->fields[1]);
        break;
      case 5: ty(d->fields[2]); ty(d->fields[3]); break;             // Tfield
      case 6: {                                                      // Tvariant
        const o::ValPtr& row = d->fields[0];
        std::vector<o::ValPtr> fields = list(row->fields[0]);
        std::stable_sort(fields.begin(), fields.end(),
                         [](const o::ValPtr& a, const o::ValPtr& b) {
                           return a->fields[0]->s < b->fields[0]->s;
                         });
        row->fields[0] = fields.empty() ? o::vint(0) : o::vlist(fields);
        ty(row->fields[1]);
        for (auto& f : fields) {
          const o::ValPtr& rf = f->fields[1];
          if (!blk(rf)) continue;
          if (rf->tag == 0) opt(rf->fields[0]);
          else if (rf->tag == 1) tys(rf->fields[1]);
        }
        if (blk(row->fields[4])) tys(row->fields[4]->fields[0]->fields[1]);
        break;
      }
      case 8: ty(d->fields[0]); tys(d->fields[1]); break;            // Tpoly
      case 9: package(d->fields[0]); break;                          // Tpackage
      case 10: package(d->fields[2]); ty(d->fields[3]); break;       // Tfunctor
      default: break;
    }
  }
  void mty(const o::ValPtr& m) {
    if (!blk(m)) return;
    if (m->tag == 1) sig(list(m->fields[0]));                        // Mty_signature
    else if (m->tag == 2) mty(m->fields[1]);                         // Mty_functor: the result
  }
  void sig(const std::vector<o::ValPtr>& items) {
    for (auto& it : items) {
      if (it->tag == 0) ty(it->fields[1]->fields[0]);                // Sig_value
      else if (it->tag == 3) mty(it->fields[2]->fields[0]);          // Sig_module
    }
  }
};

// A SAVED SIGNATURE'S type_expr IDS ARE SUBST'S CREATION ORDER (S549).
// Env.save_signature runs `Subst.signature Make_local (for_saving identity)`
// over the whole signature after `reset_for_saving` (new_id := -1), and every
// node Subst CREATES takes `decr new_id` (subst.ml newpersty): a stub for the
// node itself BEFORE its children (typexp), the children in OCaml's evaluation
// order -- constructor and record arguments RIGHT-TO-LEFT (a Tarrow's codomain
// before its domain, a Tfield's rest before its method, a Tpoly's univars
// before its body, a declaration's manifest before its kind before its
// params, a constructor's result before its arguments), `List.map`s
// left-to-right -- and a node reached twice keeps its first id (the copy
// scope's Tsubst).  Items are walked LAST-TO-FIRST (force_signature_once'
// `List.rev_map`s the renamed, reversed list) and only then, in order, the
// lazily substituted module and module-type bodies (force_signature_item), a
// functor's parameter before its result.  The emitted value graph already has
// ocamlc's shape, so the ids are assigned by a walk over it in that order;
// TyEmit's per-item ids stay on any node the walk does not reach.
struct TyIdWalk {
  long long next = -1;
  std::unordered_set<const o::Value*> seen;
  static bool blk(const o::ValPtr& v) { return v && v->k == o::Value::Block; }
  static std::vector<o::ValPtr> list(o::ValPtr v) {
    std::vector<o::ValPtr> out;
    while (blk(v) && v->fields.size() == 2) { out.push_back(v->fields[0]); v = v->fields[1]; }
    return out;
  }
  void opt(const o::ValPtr& v) { if (blk(v)) ty(v->fields[0]); }
  void tys(const o::ValPtr& l) { for (auto& t : list(l)) ty(t); }
  void package(const o::ValPtr& p) {  // {pack_path; pack_constraints}
    for (auto& c : list(p->fields[1])) ty(c->fields[1]);
  }
  void ty(const o::ValPtr& t) {
    if (!blk(t) || t->fields.size() != 4 || !seen.insert(t.get()).second) return;
    t->fields[3] = o::vint(--next);
    const o::ValPtr& d = t->fields[0];
    if (!blk(d)) return;  // Tnil
    switch (d->tag) {
      case 1: ty(d->fields[2]); ty(d->fields[1]); break;             // Tarrow
      case 2: for (auto& e : list(d->fields[0])) ty(e->fields[1]); break;  // Ttuple
      case 3: tys(d->fields[1]); break;                              // Tconstr
      case 4:                                                        // Tobject
        ty(d->fields[0]);
        if (blk(d->fields[1]) && blk(d->fields[1]->fields[0]))
          tys(d->fields[1]->fields[0]->fields[0]->fields[1]);      // ref (Some (p, tl))
        break;
      case 5: ty(d->fields[3]); ty(d->fields[2]); break;             // Tfield
      case 6: {                                                      // Tvariant
        const o::ValPtr& row = d->fields[0];
        ty(row->fields[1]);                                          // row_more
        for (auto& f : list(row->fields[0])) {
          const o::ValPtr& rf = f->fields[1];
          if (!blk(rf)) continue;
          if (rf->tag == 0) opt(rf->fields[0]);                      // RFpresent
          else if (rf->tag == 1) tys(rf->fields[1]);                 // RFeither arg_type
        }
        if (blk(row->fields[4])) tys(row->fields[4]->fields[0]->fields[1]);  // row_name
        break;
      }
      case 8: tys(d->fields[1]); ty(d->fields[0]); break;            // Tpoly
      case 9: package(d->fields[0]); break;                          // Tpackage
      case 10: ty(d->fields[3]); package(d->fields[2]); break;       // Tfunctor
      default: break;                                                // Tvar, Tunivar
    }
  }
  void cargs(const o::ValPtr& a) {
    if (!blk(a)) return;
    if (a->tag == 0) tys(a->fields[0]);                              // Cstr_tuple
    else for (auto& l : list(a->fields[0])) ty(l->fields[3]);        // Cstr_record
  }
  void decl(const o::ValPtr& d) {  // type_declaration
    opt(d->fields[4]);                                               // type_manifest
    const o::ValPtr& k = d->fields[2];
    if (blk(k) && k->tag == 1)                                       // Type_record
      for (auto& l : list(k->fields[0])) ty(l->fields[3]);
    else if (blk(k) && k->tag == 2)                                  // Type_variant
      for (auto& c : list(k->fields[0])) { opt(c->fields[2]); cargs(c->fields[1]); }
    tys(d->fields[0]);                                               // type_params
  }
  void map(const o::ValPtr& m) {  // a String Map of (_, _, type_expr), in order
    if (!blk(m)) return;
    map(m->fields[0]); ty(m->fields[2]->fields[2]); map(m->fields[3]);
  }
  void cty(const o::ValPtr& c) {  // class_type
    switch (c->tag) {
      case 0: tys(c->fields[1]); cty(c->fields[2]); break;           // Cty_constr
      case 1: {                                                      // Cty_signature
        const o::ValPtr& s = c->fields[0];
        map(s->fields[4]); map(s->fields[3]); ty(s->fields[1]); ty(s->fields[0]);
        break;
      }
      case 2: cty(c->fields[2]); ty(c->fields[1]); break;            // Cty_arrow
    }
  }
  void mty(const o::ValPtr& m) {
    if (!blk(m)) return;
    if (m->tag == 1) sig(list(m->fields[0]));                        // Mty_signature
    else if (m->tag == 2) {                                          // Mty_functor
      if (blk(m->fields[0])) mty(m->fields[0]->fields[1]);           // Named (_, mty)
      mty(m->fields[1]);
    }
  }
  void sig(const std::vector<o::ValPtr>& items) {
    for (std::size_t i = items.size(); i-- > 0;) {
      const o::ValPtr& it = items[i];
      switch (it->tag) {
        case 0: ty(it->fields[1]->fields[0]); break;                 // Sig_value
        case 1: decl(it->fields[1]); break;                          // Sig_type
        case 2: {                                                    // Sig_typext
          const o::ValPtr& e = it->fields[1];
          opt(e->fields[3]); cargs(e->fields[2]); tys(e->fields[1]);
          break;
        }
        case 5: {                                                    // Sig_class
          const o::ValPtr& c = it->fields[1];
          opt(c->fields[3]); cty(c->fields[1]); tys(c->fields[0]);
          break;
        }
        case 6: {                                                    // Sig_class_type
          const o::ValPtr& c = it->fields[1];
          decl(c->fields[3]); cty(c->fields[1]); tys(c->fields[0]);
          break;
        }
        default: break;
      }
    }
    for (auto& it : items) {
      if (it->tag == 3) mty(it->fields[2]->fields[0]);               // Sig_module
      else if (it->tag == 4 && blk(it->fields[1]->fields[0]))
        mty(it->fields[1]->fields[0]->fields[0]);                    // Sig_modtype Some
    }
  }
};

// THE STAMP TYPING GAVE EACH CONSTRUCTOR AND LABEL (S557).  `Subst.signature`
// renames a saved signature's BOUND idents (`rename_bound_idents`: the types,
// values, modules ..), which is why those stamps are the writer's contiguous
// run from `stamp_base`; a constructor's `cd_id` and a label's `ld_id` it
// copies as they are, so they keep the stamp Typedecl's `Ident.create_local`
// gave them, in the middle of whatever typing allocated around them.  The
// counting walk (cppcaml::typing_ident_count) records the counter at each
// one; this pass hands them to the items, and an item the walk did not
// record keeps the writer's placeholder.  A declaration COPIED from a .cmi
// arrives with the .cmi's own stamps (S559): those are the ones Subst kept,
// and the walk's record -- a same-named declaration of this unit's -- must
// not replace them.
static void assign_stamps_mapped(std::vector<SigItem>& items,
                                 const std::map<std::string, int>& stamps,
                                 const std::string& path) {
  auto at = [&](char kind, const std::string& name, int* out) {
    if (*out) return;
    auto it = stamps.find(uidkey(kind, path, name));
    if (it != stamps.end()) *out = it->second;
  };
  for (SigItem& it : items) {
    if (it.k == SigItem::Type) {
      for (auto& ct : it.ctors) {
        at('c', it.name + "#" + ct.name, &ct.stamp);
        for (auto& l : ct.inline_record)
          at('L', it.name + "#" + ct.name + "." + l.name, &l.stamp);
      }
      for (auto& l : it.labels) at('l', it.name + "." + l.name, &l.stamp);
    } else if (it.k == SigItem::Exception) {
      for (auto& ct : it.ctors)
        for (auto& l : ct.inline_record)
          at('L', it.name + "#" + ct.name + "." + l.name, &l.stamp);
    } else if (it.k == SigItem::Module || it.k == SigItem::Modtype) {
      assign_stamps_mapped(it.sub, stamps,
                           path.empty() ? it.name : path + "." + it.name);
    }
  }
}

std::string write_cmi(const std::string& path, const std::string& modname,
                      const std::vector<SigItem>& items_in,
                      const std::vector<Import>& imports, bool intf,
                      const std::vector<std::string>& src_files,
                      int stamp_base, bool cite,
                      const std::map<std::string, int>* uids,
                      const std::map<std::string, int>* stamps) {
  g_share.clear();              // the shared-value tables are per-cmi
  g_cmi_src_files = src_files;  // resolve position file_ids to pos_fname (emit_loc)
  std::vector<SigItem> items = items_in;  // mutable copy: uids assigned in place
  // The uids TYPING gave the declarations, when the model covered the whole
  // file (S551); its own item numbering otherwise.
  if (!uids || !assign_uids_mapped(items, modname, intf, *uids, "",
                                   /*commit=*/false) ||
      !assign_uids_mapped(items, modname, intf, *uids, "", /*commit=*/true)) {
    int uid_counter = 0;
    assign_uids(items, modname, intf, uid_counter);
  }
  if (stamps) assign_stamps_mapped(items, *stamps, "");
  std::map<std::string, bool> referenced;  // cited global unit -> needs real CRC
  int stamp = stamp_base;
  auto sig = emit_sig_items(items, referenced, stamp);
  if (!intf && !row_norm_off()) RowNormWalk{}.sig(sig);
  if (!cppcaml::dbg_env("NOTYIDS")) TyIdWalk{}.sig(sig);
  auto header = o::vblock(0, {shared_str(modname), o::vlist(sig)});
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
    if (!cite || !seen.insert(g).second) continue;
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
      if (!v.custom_raw().empty())
        return o::vcustom(v.custom_raw(), 1 + (v.custom_bsize() + 7) / 8);
      return o::vint(v.i);
    case m::Value::Kind::String: return o::vstr(v.str());
    case m::Value::Kind::Double: return o::vdbl(v.d());
    case m::Value::Kind::Block: {
      std::vector<o::ValPtr> fs;
      for (auto f : v.fields) fs.push_back(conv_value(a, f));
      return o::vblock((int)v.tag, std::move(fs));
    }
    case m::Value::Kind::DoubleArray: return o::vdblarr(v.darr());
  }
  return o::vint(0);
}

// Read a .cmi file's marshaled header `(name, signature)` and return the
// signature as an omarshal graph; `*out_name` receives the stored module name.
o::ValPtr read_cmi_sign(const std::string& path, std::string* out_name) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::vector<std::uint8_t> bytes = slurp_bytes(in);
  std::size_t off = 0;
  for (; off + 4 <= bytes.size(); ++off)
    if (bytes[off] == 0x84 && bytes[off + 1] == 0x95 && bytes[off + 2] == 0xA6 &&
        (bytes[off + 3] == 0xBE || bytes[off + 3] == 0xBF || bytes[off + 3] == 0xBD))
      break;
  if (off + 4 > bytes.size()) throw std::runtime_error("no marshal header in " + path);
  m::Arena arena;
  arena.reserve(bytes.size() * 2 / 3);  // node count ~0.5x bytes; avoid grow-and-move
  std::size_t hid = m::read_value(bytes.data(), bytes.size(), off, arena);  // (name, sign)
  arena.finalize();
  const m::Value& hv = arena[hid];
  if (hv.kind != m::Value::Kind::Block || hv.fields.size() < 2)
    throw std::runtime_error("malformed cmi header in " + path);
  if (out_name) *out_name = arena[hv.fields[0]].str();
  return conv_value(arena, hv.fields[1]);
}
}  // namespace

std::string write_packed_cmi(const std::string& path, const std::string& pack_name,
                             const std::vector<std::string>& member_cmis) {
  g_share.clear();              // the shared-value tables are per-cmi
  int stamp = 200;
  std::vector<o::ValPtr> sig_items;
  for (const auto& mc : member_cmis) {
    std::string mname;
    o::ValPtr member_sign = read_cmi_sign(mc, &mname);
    // module <Member> : sig <member_sign> end
    auto ident = ident_val(0, mname, stamp++);              // Ident.Local
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
