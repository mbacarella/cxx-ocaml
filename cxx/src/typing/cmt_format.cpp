// Port of file_formats/cmt_format.ml: see cmt_format.hpp.
//
// save_cmt marshals `clear_env binary_annots`: the typed tree rebuilt by
// Tast_mapper with every environment replaced by Env.keep_only_summary.
// The mapper builds a fresh record for each node it visits and keeps the
// leaves (locations, types, paths, idents, descriptions, the Types
// declarations) by reference; keep_only_summary returns its previous
// result when called on the same environment again.  The tree writer below
// is that mapper: it builds the node values in its shapes, calls env() in
// its evaluation order (OCaml's: constructor, tuple and record arguments
// right to left, `let`s in order, List.map left to right), and writes the
// leaves through the .cmi Writer, whose memo tables are their identities.
// The uid -> declaration index is then built as Cmt_format.index_
// declarations does (Tast_iterator's order, into a Uid.Tbl whose bucket
// layout comes from Hashtbl.hash, ported: caml_hash).
#include "cppcaml/typing/cmt_format.hpp"

#include <unistd.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <fstream>
#include <functional>
#include <unordered_map>

#include "cppcaml/blake2.hpp"
#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/persistent_env.hpp"

#include "cmi_writer.hpp"

namespace cppcaml::typing::cmt_format {

namespace o = cppcaml::omarshal;
namespace tt = typedtree;
namespace pt = parsetree;
using cmi_format::writer::EventWriter;
using cmi_format::writer::Writer;
using V = o::ValPtr;

inline constexpr const char* cmt_magic_number = "Caml1999T038";

namespace {

std::vector<std::pair<std::string_view, Location>> g_comments;
std::vector<std::string> g_argv;
std::string_view g_source_name;
struct Dep {
  DepKind k;
  Uid a, b;
};
std::vector<Dep> g_deps;  // in recording order (Uid.Deps keeps the newest first)

// ---- Hashtbl.hash (runtime/hash.c: caml_hash 10 100 0) on a value ------------
class CamlHash {
 public:
  static long hash(const V& v) {
    std::uint32_t h = 0;
    long num = 10;          // meaningful values
    const long sz = 100;    // queue size
    std::vector<const o::Value*> queue;
    queue.push_back(v.get());
    std::size_t rd = 0;
    while (rd < queue.size() && num > 0) {
      const o::Value* x = queue[rd++];
      switch (x->k) {
        case o::Value::Int: h = mix_intnat(h, 2 * x->i + 1); num--; break;
        case o::Value::Str: h = mix_string(h, x->s); num--; break;
        case o::Value::Dbl: h = mix_double(h, x->d); num--; break;
        case o::Value::Block: {
          std::uint64_t hd = (static_cast<std::uint64_t>(x->fields.size()) << 10) | static_cast<std::uint64_t>(x->tag);
          h = mix_uint32(h, static_cast<std::uint32_t>(hd));
          for (const V& f : x->fields) {
            if (static_cast<long>(queue.size()) >= sz) break;
            queue.push_back(f.get());
          }
          break;
        }
        default: break;  // customs of the uids' shapes do not occur
      }
    }
    h ^= h >> 16;
    h *= 0x85ebca6bU;
    h ^= h >> 13;
    h *= 0xc2b2ae35U;
    h ^= h >> 16;
    return static_cast<long>(h & 0x3FFFFFFFU);
  }

 private:
  static std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
  static std::uint32_t mix_uint32(std::uint32_t h, std::uint32_t d) {
    d *= 0xcc9e2d51U;
    d = rotl(d, 15);
    d *= 0x1b873593U;
    h ^= d;
    h = rotl(h, 13);
    h = h * 5 + 0xe6546b64U;
    return h;
  }
  static std::uint32_t mix_intnat(std::uint32_t h, long long d) {
    std::uint32_t n = static_cast<std::uint32_t>((d >> 32) ^ (d >> 63) ^ d);
    return mix_uint32(h, n);
  }
  static std::uint32_t mix_double(std::uint32_t h, double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, 8);
    std::uint32_t hi = static_cast<std::uint32_t>(u >> 32), lo = static_cast<std::uint32_t>(u);
    if ((hi & 0x7FF00000U) == 0x7FF00000U && (lo | (hi & 0xFFFFFU)) != 0) {
      hi = 0x7FF00000U;
      lo = 1;
    } else if (hi == 0x80000000U && lo == 0) {
      hi = 0;
    }
    h = mix_uint32(h, lo);
    return mix_uint32(h, hi);
  }
  static std::uint32_t mix_string(std::uint32_t h, const std::string& s) {
    std::size_t len = s.size(), i = 0;
    for (; i + 4 <= len; i += 4) {
      std::uint32_t w = static_cast<std::uint8_t>(s[i]) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 1])) << 8) |
                        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 2])) << 16) |
                        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 3])) << 24);
      h = mix_uint32(h, w);
    }
    std::uint32_t w = 0;
    switch (len & 3) {
      case 3: w = static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 2])) << 16; [[fallthrough]];
      case 2: w |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 1])) << 8; [[fallthrough]];
      case 1:
        w |= static_cast<std::uint8_t>(s[i]);
        h = mix_uint32(h, w);
        break;
      default: break;
    }
    return h ^ static_cast<std::uint32_t>(len);
  }
};

// ---- a Uid.Tbl (Hashtbl.Make (Uid)), as its operations build it ---------------
class UidTbl {
 public:
  struct Cell {
    V key, data;
    Cell* next;
  };
  UidTbl() : data_(16, nullptr) {}  // create 16
  void add(const V& key, const V& data) {
    std::size_t i = index(key, data_.size());
    data_[i] = new Cell{key, data, data_[i]};
    ++size_;
    if (size_ > data_.size() * 2) resize();
  }
  ~UidTbl() {
    for (Cell* c : data_)
      while (c) {
        Cell* n = c->next;
        delete c;
        c = n;
      }
  }
  // { size; data; seed; initial_size }
  V value() const {
    std::vector<V> buckets;
    for (Cell* c : data_) buckets.push_back(bucket(c));
    return o::vblock(0, {o::vint(static_cast<long long>(size_)), o::vblock(0, buckets), o::vint(0), o::vint(16)});
  }

 private:
  static std::size_t index(const V& key, std::size_t n) {
    return static_cast<std::size_t>(CamlHash::hash(key)) & (n - 1);
  }
  static V bucket(Cell* c) {
    if (!c) return o::vint(0);  // Empty
    return o::vblock(0, {c->key, c->data, bucket(c->next)});  // Cons { key; data; next }
  }
  // Hashtbl.resize / insert_all_buckets ~inplace:true: the cells keep their
  // order within each new bucket
  void resize() {
    std::size_t nsize = data_.size() * 2;
    std::vector<Cell*> ndata(nsize, nullptr), tail(nsize, nullptr);
    for (Cell* c : data_) {
      while (c) {
        Cell* next = c->next;
        std::size_t k = index(c->key, nsize);
        if (tail[k]) tail[k]->next = c;
        else ndata[k] = c;
        tail[k] = c;
        c = next;
      }
    }
    for (Cell* t : tail)
      if (t) t->next = nullptr;
    data_ = std::move(ndata);
  }
  std::vector<Cell*> data_;
  std::size_t size_ = 0;
};

// ---- the tree writer (Tast_mapper with cenv's env function) ----------------------
class TreeWriter {
 public:
  TreeWriter(Writer& w, EventWriter& ew) : w_(w), ew_(ew) {}

  // Env.keep_only_summary: {empty with summary; local_constraints; flags},
  // the previous result again for the same environment
  V env(env::t e) {
    if (e == last_env_ && last_v_) return last_v_;
    if (!empty_tbl_) empty_tbl_ = o::vblock(0, {w_.i(0), w_.i(0)});  // IdTbl.empty = TycompTbl.empty
    V t = empty_tbl_;
    V lc = ew_.path_map(e->local_constraints.root(), [&](const TypeDeclaration* d) { return w_.type_decl(d); });
    V v = o::vblock(0, {t, t, t, t, t, t, t, t, w_.i(0), ew_.summary(e->summary), lc, w_.i(0), w_.i(e->flags)});
    last_env_ = e;
    last_v_ = v;
    return v;
  }

  // ---- leaves ----
  V loc(const Location& l) { return w_.loc(l); }
  V str_loc(const pt::StrLoc& s) { return o::vblock(0, {w_.str(s.txt), loc(s.loc)}); }  // map_loc: fresh
  V opt_str_loc(const pt::OptStrLoc& s) { return o::vblock(0, {w_.opt_str(s.txt), loc(s.loc)}); }
  V lid(Longident::t l) {  // map_loc_lid's rebuilt longident
    switch (l->kind) {
      case Longident::Kind::Lident: return o::vblock(0, {w_.str(l->s)});
      case Longident::Kind::Ldot:
        return o::vblock(1, {o::vblock(0, {lid(l->l1), loc(l->l1_loc)}), o::vblock(0, {w_.str(l->s), loc(l->s_loc)})});
      case Longident::Kind::Lapply:
        return o::vblock(2, {o::vblock(0, {lid(l->l1), loc(l->l1_loc)}), o::vblock(0, {lid(l->l2), loc(l->l2_loc)})});
    }
    return w_.i(0);
  }
  V lid_loc(const pt::LidLoc& x) { return o::vblock(0, {lid(x.txt), loc(x.loc)}); }
  V opt(bool some, const std::function<V()>& f) { return some ? w_.some(f()) : w_.none(); }
  V ident_opt(Ident::t id) { return id ? w_.some(w_.ident(id)) : w_.none(); }
  V flag(long n) { return w_.i(n); }

  // the payload Ast_mapper.default_mapper rebuilds: fresh blocks, the same
  // strings, positions and locations
  V payload(const OValue* x) {
    if (x->kind != OValue::Kind::Block) return w_.ovalue(x);
    if (is_position(x)) return w_.ovalue(x);
    if (x->tag == 0 && x->fields.size() == 3 && is_position(x->fields[0]) && is_position(x->fields[1]) &&
        x->fields[2]->kind == OValue::Kind::Int)
      return loc(Location{position_of(x->fields[0]), position_of(x->fields[1]), x->fields[2]->i != 0});
    std::vector<V> fs;
    for (const OValue* f : x->fields) fs.push_back(payload(f));
    return o::vblock(static_cast<int>(x->tag), fs);
  }
  // Tast_mapper.attribute: { attr_name = map_loc; attr_payload; attr_loc }
  // (the payload as Types holds it: parsetree_ovalue)
  V attribute_(const Attribute* a) {
    return o::vblock(0, {o::vblock(0, {w_.str(a->attr_name), loc(a->attr_name_loc)}), payload(a->attr_payload),
                         loc(a->attr_loc)});
  }
  V attribute(const pt::Attribute* a) { return attribute_(pt::types_attributes(slice({a}))[0]); }
  V attributes(const pt::Attributes& as) {
    std::vector<V> xs;
    for (const Attribute* a : pt::types_attributes(as)) xs.push_back(attribute_(a));
    return o::vlist(xs);
  }
  // an attribute list kept by reference (the parsetree's, as Types holds it)
  V kept_attributes(const pt::Attributes& as) { return w_.attributes(pt::types_attributes(as)); }

  V constant(const tt::Constant& c) {
    using CK = tt::Constant::Kind;
    switch (c.kind) {
      case CK::Const_int: return o::vblock(0, {w_.i(c.i)});
      case CK::Const_char: return o::vblock(1, {w_.i(c.i)});
      case CK::Const_string: return o::vblock(2, {w_.str(c.s), loc(c.str_loc), w_.opt_str(c.delim)});
      case CK::Const_float: return o::vblock(3, {w_.str(c.s)});
      case CK::Const_int32: return o::vblock(4, {boxed(c, 4)});
      case CK::Const_int64: return o::vblock(5, {boxed(c, 8)});
      case CK::Const_nativeint: return o::vblock(6, {boxed(c, 0)});
    }
    return w_.i(0);
  }

  // ---- Data_types descriptions (kept by reference) ----
  V cstr_tag(const ConstructorTag& t) {
    using K = ConstructorTag::Kind;
    switch (t.kind) {
      case K::Cstr_constant: return o::vblock(0, {w_.i(t.n)});
      case K::Cstr_block: return o::vblock(1, {w_.i(t.n)});
      case K::Cstr_unboxed: return w_.i(0);
      case K::Cstr_extension: return o::vblock(2, {w_.path(t.ext_path), w_.b(t.ext_constant)});
    }
    return w_.i(0);
  }
  V cstr_desc(const ConstructorDescription* c) {
    return w_.shared(memo_, c, 0, [&]() -> std::vector<V> {
      return {w_.str(c->cstr_name), w_.ty(c->cstr_res), w_.tys(c->cstr_existentials), w_.tys(c->cstr_args),
              w_.i(c->cstr_arity), cstr_tag(c->cstr_tag), w_.i(c->cstr_consts), w_.i(c->cstr_nonconsts),
              w_.b(c->cstr_generalized), w_.private_flag(c->cstr_private), w_.loc(c->cstr_loc),
              w_.attributes(c->cstr_attributes), c->cstr_inlined ? w_.some(w_.type_decl(c->cstr_inlined)) : w_.none(),
              w_.uid(c->cstr_uid)};
    });
  }
  V lbl_desc(const LabelDescription* l) {
    return w_.shared(memo_, l, 0, [&]() -> std::vector<V> {
      // lbl_all: one array for all the type's labels (each label's lbl_all)
      V all = lbl_all(l->lbl_all);
      return {w_.str(l->lbl_name), w_.ty(l->lbl_res), w_.ty(l->lbl_arg), w_.mutable_flag(l->lbl_mut),
              w_.i(static_cast<long>(l->lbl_atomic)), w_.i(l->lbl_pos), all, w_.record_repr(l->lbl_repres),
              w_.private_flag(l->lbl_private), w_.loc(l->lbl_loc), w_.attributes(l->lbl_attributes),
              w_.uid(l->lbl_uid)};
    });
  }
  V lbl_all(const Slice<const LabelDescription*>& all) {
    auto key = static_cast<const void*>(all.p);
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    V arr = o::vblock(0, {});
    memo_[key] = arr;
    std::vector<V> xs;
    for (const LabelDescription* l : all) xs.push_back(lbl_desc(l));
    arr->fields = xs;
    return arr;
  }

  // ---- patterns ----
  V pat_extra(const tt::PatExtra& e) {
    using K = tt::PatExtra::Kind;
    switch (e.kind) {
      case K::Tpat_constraint: return o::vblock(0, {typ(e.cty)});
      case K::Tpat_type: return o::vblock(1, {w_.path(e.path), lid_loc(e.lid)});
      case K::Tpat_open: {  // (path, map_loc_lid, env): right to left
        V en = env(e.env);
        V l = lid_loc(e.lid);
        return o::vblock(2, {w_.path(e.path), l, en});
      }
      case K::Tpat_unpack: return o::vblock(3, {opt(e.pack != nullptr, [&] { return package_type(e.pack); })});
    }
    return w_.i(0);
  }
  V pat(const tt::Pattern* p) {
    V ploc = loc(p->pat_loc);
    V penv = env(p->pat_env);
    std::vector<V> extras;
    for (const tt::PatExtraItem& x : p->pat_extra) {  // tuple3 (pat_extra) id (attributes): right to left
      V attrs = attributes(x.attrs);
      V l = loc(x.loc);
      V e = pat_extra(x.extra);
      extras.push_back(o::vblock(0, {e, l, attrs}));
    }
    V desc = pat_desc(p);
    V pattrs = attributes(p->pat_attributes);
    return o::vblock(0, {desc, ploc, o::vlist(extras), w_.ty(p->pat_type), penv, pattrs});
  }
  V pat_desc(const tt::Pattern* p) {
    const tt::PatternDesc* d = p->pat_desc;
    using K = tt::PatternDesc::Kind;
    switch (d->kind) {
      case K::Tpat_any: return w_.i(0);
      case K::Tpat_var: {
        auto* x = tt::as<tt::Tpat_var>(d);
        return o::vblock(0, {w_.ident(x->id), str_loc(x->name), w_.uid(x->uid)});
      }
      case K::Tpat_alias: {
        auto* x = tt::as<tt::Tpat_alias>(d);
        V sl = str_loc(x->name);
        V q = pat(x->pat);
        return o::vblock(1, {q, w_.ident(x->id), sl, w_.uid(x->uid), w_.ty(x->ty)});
      }
      case K::Tpat_constant: return o::vblock(2, {constant(tt::as<tt::Tpat_constant>(d)->c)});
      case K::Tpat_tuple: {
        std::vector<V> xs;
        for (const tt::LabeledPattern& lp : tt::as<tt::Tpat_tuple>(d)->pats)
          xs.push_back(o::vblock(0, {w_.opt_str(lp.label), pat(lp.pat)}));
        return o::vblock(3, {o::vlist(xs)});
      }
      case K::Tpat_construct: {
        auto* x = tt::as<tt::Tpat_construct>(d);
        V vto = w_.none();
        if (x->annot) {  // (List.map map_loc vl, sub.typ sub cty): right to left
          V ct = typ(x->annot->cty);
          std::vector<V> vl;
          for (auto& [id, l] : x->annot->vars) vl.push_back(o::vblock(0, {w_.ident(id), loc(l)}));
          vto = w_.some(o::vblock(0, {o::vlist(vl), ct}));
        }
        std::vector<V> args;
        for (const tt::Pattern* a : x->args) args.push_back(pat(a));
        return o::vblock(4, {lid_loc(x->lid), cstr_desc(x->cstr), o::vlist(args), vto});
      }
      case K::Tpat_variant: {
        auto* x = tt::as<tt::Tpat_variant>(d);
        V arg = opt(x->arg != nullptr, [&] { return pat(x->arg); });
        V rd = w_.shared(memo_, x->row, 0, [&]() -> std::vector<V> { return {w_.row(x->row->contents)}; });
        return o::vblock(5, {w_.str(x->label), arg, rd});
      }
      case K::Tpat_record: {
        auto* x = tt::as<tt::Tpat_record>(d);
        std::vector<V> xs;
        for (const tt::RecordPatField& f : x->fields) {  // tuple3 map_loc_lid id pat: right to left
          V q = pat(f.pat);
          V l = lid_loc(f.lid);
          xs.push_back(o::vblock(0, {l, lbl_desc(f.label), q}));
        }
        return o::vblock(6, {o::vlist(xs), flag(static_cast<long>(x->closed))});
      }
      case K::Tpat_array: {
        auto* x = tt::as<tt::Tpat_array>(d);
        std::vector<V> xs;
        for (const tt::Pattern* q : x->pats) xs.push_back(pat(q));
        return o::vblock(7, {w_.mutable_flag(x->mut), o::vlist(xs)});
      }
      case K::Tpat_lazy: return o::vblock(8, {pat(tt::as<tt::Tpat_lazy>(d)->pat)});
      case K::Tpat_value: {
        // (as_computation_pattern (sub.pat sub p)).pat_desc
        return o::vblock(9, {pat(tt::as<tt::Tpat_value>(d)->pat)});
      }
      case K::Tpat_exception: return o::vblock(10, {pat(tt::as<tt::Tpat_exception>(d)->pat)});
      case K::Tpat_or: {
        auto* x = tt::as<tt::Tpat_or>(d);
        V q2 = pat(x->p2);
        V q1 = pat(x->p1);
        return o::vblock(11, {q1, q2, x->row ? w_.some(w_.row(x->row)) : w_.none()});
      }
    }
    return w_.i(0);
  }

  // ---- expressions ----
  V exp_extra(const tt::ExpExtra& e) {
    using K = tt::ExpExtra::Kind;
    switch (e.kind) {
      case K::Texp_constraint: return o::vblock(0, {typ(e.cty)});
      case K::Texp_coerce: {  // (Option.map typ cty1, typ cty2): right to left
        V t2 = typ(e.cty);
        V t1 = opt(e.from != nullptr, [&] { return typ(e.from); });
        return o::vblock(1, {t1, t2});
      }
      case K::Texp_poly: return o::vblock(2, {opt(e.cty != nullptr, [&] { return typ(e.cty); })});
      case K::Texp_newtype: return o::vblock(3, {w_.str(e.name)});
    }
    return w_.i(0);
  }
  V case_(const tt::Case* c) {  // { c_lhs; c_cont; c_guard; c_rhs }: right to left
    V rhs = expr(c->c_rhs);
    V guard = opt(c->c_guard != nullptr, [&] { return expr(c->c_guard); });
    V lhs = pat(c->c_lhs);
    return o::vblock(0, {lhs, ident_opt(c->c_cont), guard, rhs});
  }
  V cases(const Slice<const tt::Case*>& cs) {
    std::vector<V> xs;
    for (const tt::Case* c : cs) xs.push_back(case_(c));
    return o::vlist(xs);
  }
  V value_binding(const tt::ValueBinding* vb) {
    V l = loc(vb->vb_loc);
    V p = pat(vb->vb_pat);
    V e = expr(vb->vb_expr);
    V attrs = attributes(vb->vb_attributes);
    V v = o::vblock(0, {p, e, w_.i(static_cast<long>(vb->vb_rec_kind)), attrs, l});
    decls_[vb] = v;
    return v;
  }
  V value_bindings(const Slice<const tt::ValueBinding*>& vbs) {
    std::vector<V> xs;
    for (const tt::ValueBinding* vb : vbs) xs.push_back(value_binding(vb));
    return o::vlist(xs);
  }
  V apply_arg(const tt::ApplyArg& a) {
    if (a.omitted) return o::vblock(1, {w_.i(0)});  // Omitted ()
    return o::vblock(0, {expr(a.arg)});
  }
  V labeled_args(const Slice<tt::LabeledArg>& args) {
    std::vector<V> xs;
    for (const tt::LabeledArg& a : args) {  // tuple2 id (map_apply_arg): right to left
      V arg = apply_arg(a.arg);
      xs.push_back(o::vblock(0, {w_.arg_label(a.label), arg}));
    }
    return o::vlist(xs);
  }
  V function_param(const tt::FunctionParam* fp) {
    V kind;
    if (fp->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_pat) {
      kind = o::vblock(0, {pat(fp->fp_kind.pat)});
    } else {
      V p = pat(fp->fp_kind.pat);
      V e = expr(fp->fp_kind.default_);
      kind = o::vblock(1, {p, e});
    }
    V l = loc(fp->fp_loc);
    // fp_newtypes: the list kept by reference
    V nts = w_.list(fp->fp_newtypes, [&](const pt::StrLoc& s) { return o::vblock(0, {w_.str(s.txt), loc(s.loc)}); });
    return o::vblock(0, {w_.arg_label(fp->fp_arg_label), w_.ident(fp->fp_param), flag(static_cast<long>(fp->fp_partial)),
                         kind, nts, l});
  }
  V function_body(const tt::FunctionBody* b) {
    if (b->kind == tt::FunctionBody::Kind::Tfunction_body) return o::vblock(0, {expr(b->body)});
    V l = loc(b->loc);
    V cs = cases(b->cases);
    V xe = opt(b->exp_extra != nullptr, [&] { return exp_extra(*b->exp_extra); });
    V attrs = attributes(b->attributes);
    return o::vblock(1, {cs, flag(static_cast<long>(b->partial)), w_.ident(b->param), l, xe, attrs});
  }
  V binding_op(const tt::BindingOp* b) {
    V l = loc(b->bop_loc);
    V name = str_loc(b->bop_op_name);
    V e = expr(b->bop_exp);
    return o::vblock(0, {w_.path(b->bop_op_path), name, w_.value_desc(b->bop_op_val), w_.ty(b->bop_op_type), e, l});
  }
  V meth(const tt::Meth& m) {
    switch (m.kind) {
      case tt::Meth::Kind::Tmeth_name: return o::vblock(0, {w_.str(m.name)});
      case tt::Meth::Kind::Tmeth_val: return o::vblock(1, {w_.ident(m.id)});
      case tt::Meth::Kind::Tmeth_ancestor: return o::vblock(2, {w_.ident(m.id), w_.path(m.path)});
    }
    return w_.i(0);
  }
  V expr(const tt::Expression* e) {
    V l = loc(e->exp_loc);
    std::vector<V> extras;
    for (const tt::ExpExtraItem& x : e->exp_extra) {  // tuple3 extra location id: right to left
      V attrs = kept_attributes(x.attrs);  // id: the original list
      V xl = loc(x.loc);
      V xe = exp_extra(x.extra);
      extras.push_back(o::vblock(0, {xe, xl, attrs}));
    }
    V en = env(e->exp_env);
    V desc = exp_desc(e->exp_desc);
    V attrs = attributes(e->exp_attributes);
    return o::vblock(0, {desc, l, o::vlist(extras), w_.ty(e->exp_type), en, attrs});
  }
  V exprs(const Slice<const tt::Expression*>& es) {
    std::vector<V> xs;
    for (const tt::Expression* x : es) xs.push_back(expr(x));
    return o::vlist(xs);
  }
  V exp_desc(const tt::ExpressionDesc* d) {
    using K = tt::ExpressionDesc::Kind;
    switch (d->kind) {
      case K::Texp_ident: {
        auto* x = tt::as<tt::Texp_ident>(d);
        return o::vblock(0, {w_.path(x->path), lid_loc(x->lid), w_.value_desc(x->vd)});
      }
      case K::Texp_constant: return o::vblock(1, {constant(tt::as<tt::Texp_constant>(d)->c)});
      case K::Texp_let: {
        auto* x = tt::as<tt::Texp_let>(d);
        V vbs = value_bindings(x->vbs);
        V body = expr(x->body);
        return o::vblock(2, {flag(static_cast<long>(x->rec)), vbs, body});
      }
      case K::Texp_function: {
        auto* x = tt::as<tt::Texp_function>(d);
        std::vector<V> ps;
        for (const tt::FunctionParam* fp : x->params) ps.push_back(function_param(fp));
        V body = function_body(x->body);
        return o::vblock(3, {o::vlist(ps), body});
      }
      case K::Texp_apply: {
        auto* x = tt::as<tt::Texp_apply>(d);
        V args = labeled_args(x->args);
        V fn = expr(x->fn);
        return o::vblock(4, {fn, args});
      }
      case K::Texp_match: {
        auto* x = tt::as<tt::Texp_match>(d);
        V eff = cases(x->eff_cases);
        V cs = cases(x->comp_cases);
        V ex = expr(x->exp);
        return o::vblock(5, {ex, cs, eff, flag(static_cast<long>(x->partial))});
      }
      case K::Texp_try: {
        auto* x = tt::as<tt::Texp_try>(d);
        V eff = cases(x->eff_cases);
        V cs = cases(x->exn_cases);
        V ex = expr(x->exp);
        return o::vblock(6, {ex, cs, eff});
      }
      case K::Texp_tuple: {
        std::vector<V> xs;
        for (const tt::LabeledExpression& le : tt::as<tt::Texp_tuple>(d)->el)
          xs.push_back(o::vblock(0, {w_.opt_str(le.label), expr(le.exp)}));
        return o::vblock(7, {o::vlist(xs)});
      }
      case K::Texp_construct: {
        auto* x = tt::as<tt::Texp_construct>(d);
        V args = exprs(x->args);
        return o::vblock(8, {lid_loc(x->lid), cstr_desc(x->cstr), args});
      }
      case K::Texp_variant: {
        auto* x = tt::as<tt::Texp_variant>(d);
        return o::vblock(9, {w_.str(x->label), opt(x->arg != nullptr, [&] { return expr(x->arg); })});
      }
      case K::Texp_record: {
        auto* x = tt::as<tt::Texp_record>(d);
        std::vector<V> fs;
        for (const tt::RecordField& f : x->fields) {
          V def;
          if (f.def.kept) {
            def = o::vblock(0, {w_.ty(f.def.ty), w_.mutable_flag(f.def.mut)});
          } else {  // Overridden (map_loc_lid, sub.expr): right to left
            V ex = expr(f.def.exp);
            V l = lid_loc(f.def.lid);
            def = o::vblock(1, {l, ex});
          }
          fs.push_back(o::vblock(0, {lbl_desc(f.label), def}));
        }
        V ext = opt(x->extended_expression != nullptr, [&] { return expr(x->extended_expression); });
        return o::vblock(10, {o::vblock(0, fs), w_.record_repr(x->representation), ext});
      }
      case K::Texp_atomic_loc: {
        auto* x = tt::as<tt::Texp_atomic_loc>(d);
        V ex = expr(x->exp);
        return o::vblock(11, {ex, lid_loc(x->lid), lbl_desc(x->label)});
      }
      case K::Texp_field: {
        auto* x = tt::as<tt::Texp_field>(d);
        V ex = expr(x->exp);
        return o::vblock(12, {ex, lid_loc(x->lid), lbl_desc(x->label)});
      }
      case K::Texp_setfield: {
        auto* x = tt::as<tt::Texp_setfield>(d);
        V v = expr(x->value);
        V ex = expr(x->exp);
        return o::vblock(13, {ex, lid_loc(x->lid), lbl_desc(x->label), v});
      }
      case K::Texp_array: {
        auto* x = tt::as<tt::Texp_array>(d);
        return o::vblock(14, {w_.mutable_flag(x->mut), exprs(x->el)});
      }
      case K::Texp_ifthenelse: {
        auto* x = tt::as<tt::Texp_ifthenelse>(d);
        V e3 = opt(x->else_ != nullptr, [&] { return expr(x->else_); });
        V e2 = expr(x->then_);
        V e1 = expr(x->cond);
        return o::vblock(15, {e1, e2, e3});
      }
      case K::Texp_sequence: {
        auto* x = tt::as<tt::Texp_sequence>(d);
        V e2 = expr(x->e2);
        V e1 = expr(x->e1);
        return o::vblock(16, {e1, e2});
      }
      case K::Texp_while: {
        auto* x = tt::as<tt::Texp_while>(d);
        V e2 = expr(x->body);
        V e1 = expr(x->cond);
        return o::vblock(17, {e1, e2});
      }
      case K::Texp_for: {
        auto* x = tt::as<tt::Texp_for>(d);
        V e3 = expr(x->body);
        V e2 = expr(x->hi);
        V e1 = expr(x->lo);
        return o::vblock(18, {w_.ident(x->id), ppattern(x->pat), e1, e2, flag(static_cast<long>(x->dir)), e3});
      }
      case K::Texp_send: {
        auto* x = tt::as<tt::Texp_send>(d);
        return o::vblock(19, {expr(x->obj), meth(x->meth)});
      }
      case K::Texp_new: {
        auto* x = tt::as<tt::Texp_new>(d);
        return o::vblock(20, {w_.path(x->path), lid_loc(x->lid), w_.class_decl(x->decl)});
      }
      case K::Texp_instvar: {
        auto* x = tt::as<tt::Texp_instvar>(d);
        return o::vblock(21, {w_.path(x->self_path), w_.path(x->path), str_loc(x->name)});
      }
      case K::Texp_setinstvar: {
        auto* x = tt::as<tt::Texp_setinstvar>(d);
        V v = expr(x->value);
        return o::vblock(22, {w_.path(x->self_path), w_.path(x->path), str_loc(x->name), v});
      }
      case K::Texp_override: {
        auto* x = tt::as<tt::Texp_override>(d);
        std::vector<V> xs;
        for (const tt::OverrideField& f : x->fields) {  // tuple3 id map_loc expr: right to left
          V ex = expr(f.exp);
          xs.push_back(o::vblock(0, {w_.ident(f.id), str_loc(f.name), ex}));
        }
        return o::vblock(23, {w_.path(x->self_path), o::vlist(xs)});
      }
      case K::Texp_assert: {
        auto* x = tt::as<tt::Texp_assert>(d);
        return o::vblock(24, {expr(x->exp), loc(x->loc)});
      }
      case K::Texp_lazy: return o::vblock(25, {expr(tt::as<tt::Texp_lazy>(d)->exp)});
      case K::Texp_object: {
        auto* x = tt::as<tt::Texp_object>(d);
        V cs = class_structure(x->cs);
        return o::vblock(26, {cs, w_.list(x->meths, [&](std::string_view s) { return w_.str(s); })});
      }
      case K::Texp_pack: return o::vblock(27, {module_expr(tt::as<tt::Texp_pack>(d)->me)});
      case K::Texp_letop: {  // { let_; ands; param; body; partial }: right to left
        auto* x = tt::as<tt::Texp_letop>(d);
        V body = case_(x->body);
        std::vector<V> ands;
        for (const tt::BindingOp* b : x->ands) ands.push_back(binding_op(b));
        V let_ = binding_op(x->let_);
        return o::vblock(28, {let_, o::vlist(ands), w_.ident(x->param), body, flag(static_cast<long>(x->partial))});
      }
      case K::Texp_unreachable: return w_.i(0);
      case K::Texp_extension_constructor: {
        auto* x = tt::as<tt::Texp_extension_constructor>(d);
        return o::vblock(29, {lid_loc(x->lid), w_.path(x->path)});
      }
      case K::Texp_struct_item: {
        auto* x = tt::as<tt::Texp_struct_item>(d);
        V e = expr(x->body);
        V si = structure_item(x->item);
        return o::vblock(30, {si, e});
      }
    }
    return w_.i(0);
  }
  // a Parsetree.pattern kept by reference (Texp_for's)
  V ppattern(const pt::Pattern* p) {
    return w_.shared(memo_, p, 0, [&]() -> std::vector<V> {
      V desc;
      if (auto* v = pt::as<pt::Ppat_var>(p->ppat_desc)) desc = o::vblock(0, {o::vblock(0, {w_.str(v->name.txt), loc(v->name.loc)})});
      else desc = w_.i(0);  // Ppat_any (the only other for-loop pattern)
      std::vector<V> stack;
      for (const Location& l : p->ppat_loc_stack) stack.push_back(loc(l));
      return {desc, loc(p->ppat_loc), o::vlist(stack), kept_attributes(p->ppat_attributes)};
    });
  }

  // ---- core types ----
  V typ(const tt::CoreType* c) {
    V l = loc(c->ctyp_loc);
    V en = env(c->ctyp_env);
    V desc = typ_desc(c->ctyp_desc);
    V attrs = attributes(c->ctyp_attributes);
    return o::vblock(0, {desc, w_.ty(c->ctyp_type), en, l, attrs});
  }
  V typs(const Slice<const tt::CoreType*>& cs) {
    std::vector<V> xs;
    for (const tt::CoreType* c : cs) xs.push_back(typ(c));
    return o::vlist(xs);
  }
  V package_type(const tt::PackageType* p) {
    V txt = lid_loc(p->tpt_txt);
    std::vector<V> cs;
    for (auto& [l, ct] : p->tpt_constraints) {  // tuple2 map_loc_lid typ: right to left
      V t = typ(ct);
      cs.push_back(o::vblock(0, {lid_loc(l), t}));
    }
    return o::vblock(0, {w_.path(p->tpt_path), o::vlist(cs), w_.package(p->tpt_type), txt});
  }
  V row_field(const tt::RowField* r) {
    V l = loc(r->rf_loc);
    V desc;
    if (r->rf_desc.is_tag) {
      V ts = typs(r->rf_desc.types);
      desc = o::vblock(0, {str_loc(r->rf_desc.label), w_.b(r->rf_desc.constant), ts});
    } else {
      desc = o::vblock(1, {typ(r->rf_desc.inherit)});
    }
    V attrs = attributes(r->rf_attributes);
    return o::vblock(0, {desc, l, attrs});
  }
  V object_field(const tt::ObjectField* f) {
    V l = loc(f->of_loc);
    V desc;
    if (f->of_desc.is_tag) {
      V t = typ(f->of_desc.ty);
      desc = o::vblock(0, {str_loc(f->of_desc.label), t});
    } else {
      desc = o::vblock(1, {typ(f->of_desc.ty)});
    }
    V attrs = attributes(f->of_attributes);
    return o::vblock(0, {desc, l, attrs});
  }
  V typ_desc(const tt::CoreTypeDesc* d) {
    using K = tt::CoreTypeDesc::Kind;
    switch (d->kind) {
      case K::Ttyp_any: return w_.i(0);
      case K::Ttyp_var: return o::vblock(0, {w_.str(tt::as<tt::Ttyp_var>(d)->name)});
      case K::Ttyp_arrow: {
        auto* x = tt::as<tt::Ttyp_arrow>(d);
        V t2 = typ(x->t2);
        V t1 = typ(x->t1);
        return o::vblock(1, {w_.arg_label(x->label), t1, t2});
      }
      case K::Ttyp_tuple: {
        std::vector<V> xs;
        for (const tt::LabeledCoreType& lt : tt::as<tt::Ttyp_tuple>(d)->tl)
          xs.push_back(o::vblock(0, {w_.opt_str(lt.label), typ(lt.ty)}));
        return o::vblock(2, {o::vlist(xs)});
      }
      case K::Ttyp_constr: {
        auto* x = tt::as<tt::Ttyp_constr>(d);
        V args = typs(x->args);
        return o::vblock(3, {w_.path(x->path), lid_loc(x->lid), args});
      }
      case K::Ttyp_object: {
        auto* x = tt::as<tt::Ttyp_object>(d);
        std::vector<V> xs;
        for (const tt::ObjectField* f : x->fields) xs.push_back(object_field(f));
        return o::vblock(4, {o::vlist(xs), flag(static_cast<long>(x->closed))});
      }
      case K::Ttyp_class: {
        auto* x = tt::as<tt::Ttyp_class>(d);
        V args = typs(x->args);
        return o::vblock(5, {w_.path(x->path), lid_loc(x->lid), args});
      }
      case K::Ttyp_alias: {
        auto* x = tt::as<tt::Ttyp_alias>(d);
        V t = typ(x->ty);
        return o::vblock(6, {t, o::vblock(0, {w_.str(x->name.txt), loc(x->name.loc)})});
      }
      case K::Ttyp_variant: {
        auto* x = tt::as<tt::Ttyp_variant>(d);
        std::vector<V> xs;
        for (const tt::RowField* r : x->fields) xs.push_back(row_field(r));
        V labels = x->has_labels ? w_.some(w_.list(x->labels, [&](std::string_view s) { return w_.str(s); })) : w_.none();
        return o::vblock(7, {o::vlist(xs), flag(static_cast<long>(x->closed)), labels});
      }
      case K::Ttyp_poly: {
        auto* x = tt::as<tt::Ttyp_poly>(d);
        V t = typ(x->ty);
        return o::vblock(8, {w_.list(x->vars, [&](std::string_view s) { return w_.str(s); }), t});
      }
      case K::Ttyp_package: return o::vblock(9, {package_type(tt::as<tt::Ttyp_package>(d)->pack)});
      case K::Ttyp_open: {
        auto* x = tt::as<tt::Ttyp_open>(d);
        V t = typ(x->ty);
        return o::vblock(10, {w_.path(x->path), lid_loc(x->lid), t});
      }
      case K::Ttyp_functor: {
        auto* x = tt::as<tt::Ttyp_functor>(d);
        V t = typ(x->ty);
        V p = package_type(x->pack);
        return o::vblock(11, {w_.arg_label(x->label), o::vblock(0, {w_.ident(x->id), loc(x->id_loc)}), p, t});
      }
    }
    return w_.i(0);
  }

  // ---- declarations ----
  V type_param(const tt::TypeParam& p) {  // tuple2 (typ) id: right to left
    // the (variance, injectivity) pair: parser.mly's type_variance returns
    // literal tuples, static constants (one per value)
    auto [it, fresh] = var_inj_.try_emplace(std::make_pair(static_cast<int>(p.variance), static_cast<int>(p.injectivity)), nullptr);
    if (fresh) it->second = o::vblock(0, {w_.i(static_cast<long>(p.variance)), w_.i(static_cast<long>(p.injectivity))});
    return o::vblock(0, {typ(p.ty), it->second});
  }
  V type_params(const Slice<tt::TypeParam>& ps) {
    std::vector<V> xs;
    for (const tt::TypeParam& p : ps) xs.push_back(type_param(p));
    return o::vlist(xs);
  }
  V value_description(const tt::TValueDescription* v) {
    V l = loc(v->val_loc);
    V name = str_loc(v->val_name);
    V desc = typ(v->val_desc);
    V attrs = attributes(v->val_attributes);
    V r = o::vblock(0, {w_.ident(v->val_id), name, desc, w_.value_desc(v->val_val), l, attrs});
    decls_[v] = r;
    return r;
  }
  V primitive_description(const tt::TPrimitiveDescription* p) {
    V l = loc(p->prim_loc);
    V name = str_loc(p->prim_name);
    V kind;
    if (p->prim_kind.kind == tt::PrimitiveKind::Kind::Tprim_decl) {
      V t = typ(p->prim_kind.cty);
      kind = o::vblock(0, {t, w_.list(p->prim_kind.prims, [&](std::string_view s) { return w_.str(s); })});
    } else {  // (Option.map typ, path, map_loc_lid): right to left
      V l2 = lid_loc(p->prim_kind.lid);
      V t = opt(p->prim_kind.cty != nullptr, [&] { return typ(p->prim_kind.cty); });
      kind = o::vblock(1, {t, w_.path(p->prim_kind.path), l2});
    }
    V attrs = attributes(p->prim_attributes);
    V r = o::vblock(0, {w_.ident(p->prim_id), name, kind, w_.value_desc(p->prim_val), l, attrs});
    decls_[p] = r;
    return r;
  }
  V label_decl(const tt::TLabelDeclaration* ld) {
    V l = loc(ld->ld_loc);
    V name = str_loc(ld->ld_name);
    V t = typ(ld->ld_type);
    V attrs = attributes(ld->ld_attributes);
    V r = o::vblock(0, {w_.ident(ld->ld_id), name, w_.uid(ld->ld_uid), w_.mutable_flag(ld->ld_mutable),
                        w_.i(static_cast<long>(ld->ld_atomic)), t, l, attrs});
    decls_[ld] = r;
    return r;
  }
  V constructor_args(const tt::TConstructorArguments& a) {
    if (!a.is_record) return o::vblock(0, {typs(a.tuple)});
    std::vector<V> xs;
    for (const tt::TLabelDeclaration* ld : a.record) xs.push_back(label_decl(ld));
    return o::vblock(1, {o::vlist(xs)});
  }
  V constructor_decl(const tt::TConstructorDeclaration* cd) {
    V l = loc(cd->cd_loc);
    V name = str_loc(cd->cd_name);
    std::vector<V> vars;
    for (const pt::StrLoc& s : cd->cd_vars) vars.push_back(str_loc(s));
    V args = constructor_args(cd->cd_args);
    V res = opt(cd->cd_res != nullptr, [&] { return typ(cd->cd_res); });
    V attrs = attributes(cd->cd_attributes);
    V r = o::vblock(0, {w_.ident(cd->cd_id), name, w_.uid(cd->cd_uid), o::vlist(vars), args, res, l, attrs});
    decls_[cd] = r;
    return r;
  }
  V type_kind(const tt::TTypeKind& k) {
    using K = tt::TTypeKind::Kind;
    switch (k.kind) {
      case K::Ttype_abstract: return w_.i(0);
      case K::Ttype_open: return w_.i(1);
      case K::Ttype_variant: {
        std::vector<V> xs;
        for (const tt::TConstructorDeclaration* cd : k.constructors) xs.push_back(constructor_decl(cd));
        return o::vblock(0, {o::vlist(xs)});
      }
      case K::Ttype_record: {
        std::vector<V> xs;
        for (const tt::TLabelDeclaration* ld : k.labels) xs.push_back(label_decl(ld));
        return o::vblock(1, {o::vlist(xs)});
      }
      case K::Ttype_external: return o::vblock(2, {w_.str(k.external)});
    }
    return w_.i(0);
  }
  V type_declaration(const tt::TTypeDeclaration* td) {
    V l = loc(td->typ_loc);
    V name = str_loc(td->typ_name);
    std::vector<V> cs;
    for (const tt::TypeConstraintItem& c : td->typ_constraints) {  // tuple3 typ typ location: right to left
      V cl = loc(c.loc);
      V t2 = typ(c.t2);
      V t1 = typ(c.t1);
      cs.push_back(o::vblock(0, {t1, t2, cl}));
    }
    V kind = type_kind(td->typ_kind);
    V manifest = opt(td->typ_manifest != nullptr, [&] { return typ(td->typ_manifest); });
    V params = type_params(td->typ_params);
    V attrs = attributes(td->typ_attributes);
    V r = o::vblock(0, {w_.ident(td->typ_id), name, params, w_.type_decl(td->typ_type), o::vlist(cs), kind,
                        w_.private_flag(td->typ_private), manifest, l, attrs});
    decls_[td] = r;
    return r;
  }
  V type_declarations(const Slice<const tt::TTypeDeclaration*>& ds) {
    std::vector<V> xs;
    for (const tt::TTypeDeclaration* td : ds) xs.push_back(type_declaration(td));
    return o::vlist(xs);
  }
  V extension_constructor(const tt::TExtensionConstructor* ec) {
    V l = loc(ec->ext_loc);
    V name = str_loc(ec->ext_name);
    V kind;
    if (ec->ext_kind.kind == tt::TExtensionConstructorKind::Kind::Text_decl) {
      // Text_decl (List.map map_loc ids, constructor_args, Option.map typ): right to left
      V res = opt(ec->ext_kind.res != nullptr, [&] { return typ(ec->ext_kind.res); });
      V args = constructor_args(ec->ext_kind.args);
      std::vector<V> ids;
      for (const pt::StrLoc& s : ec->ext_kind.vars) ids.push_back(str_loc(s));
      kind = o::vblock(0, {o::vlist(ids), args, res});
    } else {
      kind = o::vblock(1, {w_.path(ec->ext_kind.path), lid_loc(ec->ext_kind.lid)});
    }
    V attrs = attributes(ec->ext_attributes);
    V r = o::vblock(0, {w_.ident(ec->ext_id), name, w_.ext_constr(ec->ext_type), kind, l, attrs});
    decls_[ec] = r;
    return r;
  }
  V type_extension(const tt::TTypeExtension* te) {
    V l = loc(te->tyext_loc);
    V txt = lid_loc(te->tyext_txt);
    V params = type_params(te->tyext_params);
    std::vector<V> cs;
    for (const tt::TExtensionConstructor* ec : te->tyext_constructors) cs.push_back(extension_constructor(ec));
    V attrs = attributes(te->tyext_attributes);
    return o::vblock(0, {w_.path(te->tyext_path), txt, params, o::vlist(cs), w_.private_flag(te->tyext_private), l,
                         attrs});
  }
  V type_exception(const tt::TTypeException* te) {
    V l = loc(te->tyexn_loc);
    V c = extension_constructor(te->tyexn_constructor);
    V attrs = attributes(te->tyexn_attributes);
    return o::vblock(0, {c, l, attrs});
  }

  // ---- module language ----
  V module_coercion(const tt::ModuleCoercion* c) {
    using K = tt::ModuleCoercion::Kind;
    switch (c->kind) {
      case K::Tcoerce_none: return w_.i(0);
      case K::Tcoerce_structure: {
        std::vector<V> l1, l2;
        for (const tt::PosCoercion& p : c->pos_cc) l1.push_back(o::vblock(0, {w_.i(p.pos), module_coercion(p.cc)}));
        for (const tt::IdPosCoercion& p : c->id_pos_cc)
          l2.push_back(o::vblock(0, {w_.ident(p.id), w_.i(p.pos), module_coercion(p.cc)}));
        return o::vblock(0, {o::vlist(l1), o::vlist(l2)});
      }
      case K::Tcoerce_functor: {
        V c2 = module_coercion(c->res);
        V c1 = module_coercion(c->arg);
        return o::vblock(1, {c1, c2});
      }
      case K::Tcoerce_primitive: {
        const tt::PrimitiveCoercion* pc = c->prim;
        V en = env(pc->pc_env);
        return o::vblock(2, {o::vblock(0, {w_.prim_desc(pc->pc_desc), w_.ty(pc->pc_type), en, loc(pc->pc_loc)})});
      }
      case K::Tcoerce_alias: {  // (env, p, coercion c1): right to left
        V c1 = module_coercion(c->alias_coercion);
        V en = env(c->alias_env);
        return o::vblock(3, {en, w_.path(c->alias_path), c1});
      }
    }
    return w_.i(0);
  }
  V functor_parameter(const tt::FunctorParameter& p) {
    if (p.is_unit) return w_.i(0);
    V mt = module_type(p.mty);
    V id = p.id ? w_.some_shared(p.some_obj, [&] { return w_.ident(p.id); }) : w_.none();
    return o::vblock(0, {id, opt_str_loc(p.name), mt});
  }
  V module_expr(const tt::ModuleExpr* m) {
    V l = loc(m->mod_loc);
    V en = env(m->mod_env);
    V desc = module_expr_desc(m->mod_desc);
    V attrs = attributes(m->mod_attributes);
    return o::vblock(0, {desc, l, w_.module_type(m->mod_type), en, attrs});
  }
  V module_expr_desc(const tt::ModuleExprDesc* d) {
    using K = tt::ModuleExprDesc::Kind;
    switch (d->kind) {
      case K::Tmod_ident: {
        auto* x = tt::as<tt::Tmod_ident>(d);
        return o::vblock(0, {w_.path(x->path), lid_loc(x->lid)});
      }
      case K::Tmod_structure: return o::vblock(1, {structure(tt::as<tt::Tmod_structure>(d)->str)});
      case K::Tmod_functor: {
        auto* x = tt::as<tt::Tmod_functor>(d);
        V body = module_expr(x->body);
        V param = functor_parameter(x->param);
        return o::vblock(2, {param, body});
      }
      case K::Tmod_apply: {
        auto* x = tt::as<tt::Tmod_apply>(d);
        V c = module_coercion(x->coercion);
        V arg = module_expr(x->arg);
        V fn = module_expr(x->fn);
        return o::vblock(3, {fn, arg, c});
      }
      case K::Tmod_apply_unit: return o::vblock(4, {module_expr(tt::as<tt::Tmod_apply_unit>(d)->fn)});
      case K::Tmod_constraint: {
        auto* x = tt::as<tt::Tmod_constraint>(d);
        V c = module_coercion(x->coercion);
        V cstr = x->explicit_mty ? o::vblock(0, {module_type(x->explicit_mty)}) : w_.i(0);
        V me = module_expr(x->me);
        return o::vblock(5, {me, w_.module_type(x->mty), cstr, c});
      }
      case K::Tmod_unpack: {
        auto* x = tt::as<tt::Tmod_unpack>(d);
        return o::vblock(6, {expr(x->exp), w_.module_type(x->mty)});
      }
    }
    return w_.i(0);
  }
  V structure(const tt::Structure* s) {  // { str_items; str_type; str_final_env }: right to left
    V en = env(s->str_final_env);
    std::vector<V> items;
    for (const tt::StructureItem* it : s->str_items) items.push_back(structure_item(it));
    return o::vblock(0, {o::vlist(items), w_.signature(s->str_type), en});
  }
  V module_binding(const tt::ModuleBinding* mb) {
    V l = loc(mb->mb_loc);
    V name = opt_str_loc(mb->mb_name);
    V me = module_expr(mb->mb_expr);
    V attrs = attributes(mb->mb_attributes);
    V r = o::vblock(0, {ident_opt(mb->mb_id), name, w_.uid(mb->mb_uid),
                        w_.i(mb->mb_presence == ModulePresence::Mp_present ? 0 : 1), me, attrs, l});
    decls_[mb] = r;
    return r;
  }
  V module_type_declaration(const tt::TModuleTypeDeclaration* mtd) {
    V l = loc(mtd->mtd_loc);
    V name = str_loc(mtd->mtd_name);
    V mt = opt(mtd->mtd_type != nullptr, [&] { return module_type(mtd->mtd_type); });
    V attrs = attributes(mtd->mtd_attributes);
    V r = o::vblock(0, {w_.ident(mtd->mtd_id), name, w_.uid(mtd->mtd_uid), mt, attrs, l});
    decls_[mtd] = r;
    return r;
  }
  V open_declaration(const tt::OpenDeclaration* od) {  // record: right to left
    V attrs = attributes(od->open_attributes);
    V l = loc(od->open_loc);
    V en = env(od->open_env);
    V me = module_expr(od->open_expr);
    return o::vblock(0, {me, w_.signature(od->open_bound_items), flag(static_cast<long>(od->open_override)), en, l,
                         attrs});
  }
  V open_description(const tt::OpenDescription* od) {  // record: right to left
    V attrs = attributes(od->open_attributes);
    V l = loc(od->open_loc);
    V en = env(od->open_env);
    V e = o::vblock(0, {w_.path(od->open_expr.path), lid_loc(od->open_expr.lid)});  // tuple2 id map_loc_lid
    return o::vblock(0, {e, w_.signature(od->open_bound_items), flag(static_cast<long>(od->open_override)), en, l,
                         attrs});
  }
  template <class A, class F>
  V include_infos(const tt::IncludeInfos<A>* incl, F&& f) {
    V l = loc(incl->incl_loc);
    V attrs = attributes(incl->incl_attributes);
    V m = f(incl->incl_mod);
    return o::vblock(0, {m, w_.signature(incl->incl_type), l, attrs});
  }
  V structure_item(const tt::StructureItem* it) {
    V l = loc(it->str_loc);
    V en = env(it->str_env);
    V desc = structure_item_desc(it->str_desc);
    return o::vblock(0, {desc, l, en});
  }
  V structure_item_desc(const tt::StructureItemDesc* d) {
    using K = tt::StructureItemDesc::Kind;
    switch (d->kind) {
      case K::Tstr_eval: {
        auto* x = tt::as<tt::Tstr_eval>(d);
        V attrs = attributes(x->attrs);
        V e = expr(x->exp);
        return o::vblock(0, {e, attrs});
      }
      case K::Tstr_value: {
        auto* x = tt::as<tt::Tstr_value>(d);
        return o::vblock(1, {flag(static_cast<long>(x->rec)), value_bindings(x->vbs)});
      }
      case K::Tstr_primitive: return o::vblock(2, {primitive_description(tt::as<tt::Tstr_primitive>(d)->pd)});
      case K::Tstr_type: {
        auto* x = tt::as<tt::Tstr_type>(d);
        return o::vblock(3, {flag(static_cast<long>(x->rec)), type_declarations(x->decls)});
      }
      case K::Tstr_typext: return o::vblock(4, {type_extension(tt::as<tt::Tstr_typext>(d)->ext)});
      case K::Tstr_exception: return o::vblock(5, {type_exception(tt::as<tt::Tstr_exception>(d)->exn)});
      case K::Tstr_module: return o::vblock(6, {module_binding(tt::as<tt::Tstr_module>(d)->mb)});
      case K::Tstr_recmodule: {
        std::vector<V> xs;
        for (const tt::ModuleBinding* mb : tt::as<tt::Tstr_recmodule>(d)->mbs) xs.push_back(module_binding(mb));
        return o::vblock(7, {o::vlist(xs)});
      }
      case K::Tstr_modtype: return o::vblock(8, {module_type_declaration(tt::as<tt::Tstr_modtype>(d)->mtd)});
      case K::Tstr_open: return o::vblock(9, {open_declaration(tt::as<tt::Tstr_open>(d)->od)});
      case K::Tstr_class: {
        std::vector<V> xs;
        for (const tt::ClassDeclarationItem& c : tt::as<tt::Tstr_class>(d)->classes) {
          V names = w_.list(c.names, [&](std::string_view s) { return w_.str(s); });  // id: the original list
          xs.push_back(o::vblock(0, {class_declaration(c.decl), names}));
        }
        return o::vblock(10, {o::vlist(xs)});
      }
      case K::Tstr_class_type: {
        std::vector<V> xs;
        for (const tt::ClassTypeDeclarationItem& c : tt::as<tt::Tstr_class_type>(d)->classes) {
          V ctd = class_type_declaration(c.decl);  // tuple3 id map_loc (class_type_declaration): right to left
          V name = str_loc(c.name);
          xs.push_back(o::vblock(0, {w_.ident(c.id), name, ctd}));
        }
        return o::vblock(11, {o::vlist(xs)});
      }
      case K::Tstr_include:
        return o::vblock(12, {include_infos(tt::as<tt::Tstr_include>(d)->incl,
                                            [&](const tt::ModuleExpr* me) { return module_expr(me); })});
      case K::Tstr_attribute: return o::vblock(13, {attribute(tt::as<tt::Tstr_attribute>(d)->attr)});
    }
    return w_.i(0);
  }
  V with_constraint(const tt::WithConstraint& c) {
    using K = tt::WithConstraint::Kind;
    switch (c.kind) {
      case K::Twith_type: return o::vblock(0, {type_declaration(c.decl)});
      case K::Twith_module: return o::vblock(1, {w_.path(c.path), lid_loc(c.lid)});
      case K::Twith_modtype: return o::vblock(2, {module_type(c.mty)});
      case K::Twith_typesubst: return o::vblock(3, {type_declaration(c.decl)});
      case K::Twith_modsubst: return o::vblock(4, {w_.path(c.path), lid_loc(c.lid)});
      case K::Twith_modtypesubst: return o::vblock(5, {module_type(c.mty)});
    }
    return w_.i(0);
  }
  V module_type(const tt::ModuleType* m) {
    V l = loc(m->mty_loc);
    V en = env(m->mty_env);
    V desc = module_type_desc(m->mty_desc);
    V attrs = attributes(m->mty_attributes);
    return o::vblock(0, {desc, w_.module_type(m->mty_type), en, l, attrs});
  }
  V module_type_desc(const tt::ModuleTypeDesc* d) {
    using K = tt::ModuleTypeDesc::Kind;
    switch (d->kind) {
      case K::Tmty_ident: {
        auto* x = tt::as<tt::Tmty_ident>(d);
        return o::vblock(0, {w_.path(x->path), lid_loc(x->lid)});
      }
      case K::Tmty_signature: return o::vblock(1, {signature(tt::as<tt::Tmty_signature>(d)->sig)});
      case K::Tmty_functor: {
        auto* x = tt::as<tt::Tmty_functor>(d);
        V body = module_type(x->body);
        V param = functor_parameter(x->param);
        return o::vblock(2, {param, body});
      }
      case K::Tmty_with: {
        auto* x = tt::as<tt::Tmty_with>(d);
        std::vector<V> cs;
        for (const tt::WithConstraintItem& c : x->cstrs) {  // tuple3 id map_loc_lid with_constraint: right to left
          V wc = with_constraint(c.cstr);
          cs.push_back(o::vblock(0, {w_.path(c.path), lid_loc(c.lid), wc}));
        }
        V mt = module_type(x->mty);
        return o::vblock(3, {mt, o::vlist(cs)});
      }
      case K::Tmty_typeof: return o::vblock(4, {module_expr(tt::as<tt::Tmty_typeof>(d)->me)});
      case K::Tmty_alias: {
        auto* x = tt::as<tt::Tmty_alias>(d);
        return o::vblock(5, {w_.path(x->path), lid_loc(x->lid)});
      }
    }
    return w_.i(0);
  }
  V signature(const tt::Signature* s) {
    V en = env(s->sig_final_env);
    std::vector<V> items;
    for (const tt::SignatureItem* it : s->sig_items) items.push_back(signature_item(it));
    return o::vblock(0, {o::vlist(items), w_.signature(s->sig_type), en});
  }
  V module_declaration(const tt::TModuleDeclaration* md) {
    V l = loc(md->md_loc);
    V name = opt_str_loc(md->md_name);
    V mt = module_type(md->md_type);
    V attrs = attributes(md->md_attributes);
    V r = o::vblock(0, {ident_opt(md->md_id), name, w_.uid(md->md_uid),
                        w_.i(md->md_presence == ModulePresence::Mp_present ? 0 : 1), mt, attrs, l});
    decls_[md] = r;
    return r;
  }
  V module_substitution(const tt::TModuleSubstitution* ms) {
    V l = loc(ms->ms_loc);
    V name = str_loc(ms->ms_name);
    V txt = lid_loc(ms->ms_txt);
    V attrs = attributes(ms->ms_attributes);
    V r = o::vblock(0, {w_.ident(ms->ms_id), name, w_.uid(ms->ms_uid), w_.path(ms->ms_manifest), txt, attrs, l});
    decls_[ms] = r;
    return r;
  }
  V signature_item(const tt::SignatureItem* it) {
    V l = loc(it->sig_loc);
    V en = env(it->sig_env);
    V desc = signature_item_desc(it->sig_desc);
    return o::vblock(0, {desc, en, l});
  }
  V signature_item_desc(const tt::SignatureItemDesc* d) {
    using K = tt::SignatureItemDesc::Kind;
    switch (d->kind) {
      case K::Tsig_value: return o::vblock(0, {value_description(tt::as<tt::Tsig_value>(d)->vd)});
      case K::Tsig_primitive: return o::vblock(1, {primitive_description(tt::as<tt::Tsig_primitive>(d)->pd)});
      case K::Tsig_type: {
        auto* x = tt::as<tt::Tsig_type>(d);
        return o::vblock(2, {flag(static_cast<long>(x->rec)), type_declarations(x->decls)});
      }
      case K::Tsig_typesubst: return o::vblock(3, {type_declarations(tt::as<tt::Tsig_typesubst>(d)->decls)});
      case K::Tsig_typext: return o::vblock(4, {type_extension(tt::as<tt::Tsig_typext>(d)->ext)});
      case K::Tsig_exception: return o::vblock(5, {type_exception(tt::as<tt::Tsig_exception>(d)->exn)});
      case K::Tsig_module: return o::vblock(6, {module_declaration(tt::as<tt::Tsig_module>(d)->md)});
      case K::Tsig_modsubst: return o::vblock(7, {module_substitution(tt::as<tt::Tsig_modsubst>(d)->ms)});
      case K::Tsig_recmodule: {
        std::vector<V> xs;
        for (const tt::TModuleDeclaration* md : tt::as<tt::Tsig_recmodule>(d)->mds) xs.push_back(module_declaration(md));
        return o::vblock(8, {o::vlist(xs)});
      }
      case K::Tsig_modtype: return o::vblock(9, {module_type_declaration(tt::as<tt::Tsig_modtype>(d)->mtd)});
      case K::Tsig_modtypesubst:
        return o::vblock(10, {module_type_declaration(tt::as<tt::Tsig_modtypesubst>(d)->mtd)});
      case K::Tsig_open: return o::vblock(11, {open_description(tt::as<tt::Tsig_open>(d)->od)});
      case K::Tsig_include:
        return o::vblock(12, {include_infos(tt::as<tt::Tsig_include>(d)->incl,
                                            [&](const tt::ModuleType* mt) { return module_type(mt); })});
      case K::Tsig_class: {
        std::vector<V> xs;
        for (const tt::TClassDescription* c : tt::as<tt::Tsig_class>(d)->classes) xs.push_back(class_description(c));
        return o::vblock(13, {o::vlist(xs)});
      }
      case K::Tsig_class_type: {
        std::vector<V> xs;
        for (const tt::TClassTypeDeclaration* c : tt::as<tt::Tsig_class_type>(d)->classes)
          xs.push_back(class_type_declaration(c));
        return o::vblock(14, {o::vlist(xs)});
      }
      case K::Tsig_attribute: return o::vblock(15, {attribute(tt::as<tt::Tsig_attribute>(d)->attr)});
    }
    return w_.i(0);
  }

  // ---- class language ----
  // class_infos: {x with ci_loc; ci_id_name; ci_params; ci_expr; ci_attributes}:
  // the new fields right to left in declaration order
  template <class A, class F>
  V class_infos(const tt::ClassInfos<A>* ci, F&& f) {
    V attrs = attributes(ci->ci_attributes);
    V l = loc(ci->ci_loc);
    V e = f(ci->ci_expr);
    V name = str_loc(ci->ci_id_name);
    V params = type_params(ci->ci_params);
    return o::vblock(0, {w_.virtual_flag(ci->ci_virt), params, name, w_.ident(ci->ci_id_class),
                         w_.ident(ci->ci_id_class_type), w_.ident(ci->ci_id_object), e, w_.class_decl(ci->ci_decl),
                         w_.cltype_decl(ci->ci_type_decl), l, attrs});
  }
  V class_declaration(const tt::TClassDeclaration* cd) {
    V r = class_infos(cd, [&](const tt::ClassExpr* ce) { return class_expr(ce); });
    decls_[cd] = r;
    return r;
  }
  V class_description(const tt::TClassDescription* cd) {
    V r = class_infos(cd, [&](const tt::ClassType* ct) { return class_type(ct); });
    decls_[cd] = r;
    return r;
  }
  V class_type_declaration(const tt::TClassTypeDeclaration* cd) { return class_description(cd); }
  V ident_expressions(const Slice<tt::IdentExpression>& xs) {
    std::vector<V> out;
    for (const tt::IdentExpression& x : xs) out.push_back(o::vblock(0, {w_.ident(x.id), expr(x.exp)}));
    return o::vlist(out);
  }
  V class_expr(const tt::ClassExpr* c) {
    V l = loc(c->cl_loc);
    V en = env(c->cl_env);
    V desc = class_expr_desc(c->cl_desc);
    V attrs = attributes(c->cl_attributes);
    return o::vblock(0, {desc, l, w_.class_type(c->cl_type), en, attrs});
  }
  V class_expr_desc(const tt::ClassExprDesc* d) {
    using K = tt::ClassExprDesc::Kind;
    switch (d->kind) {
      case K::Tcl_ident: {
        auto* x = tt::as<tt::Tcl_ident>(d);
        V args = typs(x->args);
        return o::vblock(0, {w_.path(x->path), lid_loc(x->lid), args});
      }
      case K::Tcl_structure: return o::vblock(1, {class_structure(tt::as<tt::Tcl_structure>(d)->cs)});
      case K::Tcl_fun: {
        auto* x = tt::as<tt::Tcl_fun>(d);
        V ce = class_expr(x->ce);
        V priv = ident_expressions(x->args);
        V p = pat(x->pat);
        return o::vblock(2, {w_.arg_label(x->label), p, priv, ce, flag(static_cast<long>(x->partial))});
      }
      case K::Tcl_apply: {
        auto* x = tt::as<tt::Tcl_apply>(d);
        V args = labeled_args(x->args);
        V ce = class_expr(x->ce);
        return o::vblock(3, {ce, args});
      }
      case K::Tcl_let: {
        auto* x = tt::as<tt::Tcl_let>(d);
        V vbs = value_bindings(x->vbs);
        V ce = class_expr(x->ce);
        V ivars = ident_expressions(x->vals);
        return o::vblock(4, {flag(static_cast<long>(x->rec)), vbs, ivars, ce});
      }
      case K::Tcl_constraint: {
        auto* x = tt::as<tt::Tcl_constraint>(d);
        V clty = opt(x->cty != nullptr, [&] { return class_type(x->cty); });
        V ce = class_expr(x->ce);
        auto strs = [&](const Slice<std::string_view>& l) { return w_.list(l, [&](std::string_view s) { return w_.str(s); }); };
        return o::vblock(5, {ce, clty, strs(x->vals), strs(x->meths), meth_set(x->concrete_meths)});
      }
      case K::Tcl_open: {
        auto* x = tt::as<tt::Tcl_open>(d);
        V ce = class_expr(x->ce);
        V od = open_description(x->od);
        return o::vblock(6, {od, ce});
      }
    }
    return w_.i(0);
  }
  // Types.MethSet.t (a Set.Make (String)): the balanced tree of adding the
  // elements in order (the port keeps the elements, sorted)
  V meth_set(const Slice<std::string_view>& xs) {
    struct N {
      N* l;
      std::string_view v;
      N* r;
      int h;
    };
    std::vector<std::unique_ptr<N>> pool;
    auto height = [](N* n) { return n ? n->h : 0; };
    auto create = [&](N* l, std::string_view v, N* r) {
      int hl = height(l), hr = height(r);
      pool.push_back(std::make_unique<N>(N{l, v, r, hl >= hr ? hl + 1 : hr + 1}));
      return pool.back().get();
    };
    std::function<N*(N*, std::string_view, N*)> bal = [&](N* l, std::string_view v, N* r) -> N* {
      int hl = height(l), hr = height(r);
      if (hl > hr + 2) {
        N* ll = l->l;
        N* lr = l->r;
        if (height(ll) >= height(lr)) return create(ll, l->v, create(lr, v, r));
        return create(create(ll, l->v, lr->l), lr->v, create(lr->r, v, r));
      }
      if (hr > hl + 2) {
        N* rl = r->l;
        N* rr = r->r;
        if (height(rr) >= height(rl)) return create(create(l, v, rl), r->v, rr);
        return create(create(l, v, rl->l), rl->v, create(rl->r, r->v, rr));
      }
      return create(l, v, r);
    };
    std::function<N*(std::string_view, N*)> add = [&](std::string_view x, N* t) -> N* {
      if (!t) return create(nullptr, x, nullptr);
      int c = x.compare(t->v);
      if (c == 0) return t;
      if (c < 0) return bal(add(x, t->l), t->v, t->r);
      return bal(t->l, t->v, add(x, t->r));
    };
    N* t = nullptr;
    for (std::string_view x : xs) t = add(x, t);
    std::function<V(N*)> out = [&](N* n) -> V {
      if (!n) return w_.i(0);
      return o::vblock(0, {out(n->l), w_.str(n->v), out(n->r), w_.i(n->h)});
    };
    return out(t);
  }
  V class_structure(const tt::ClassStructure* cs) {
    V self = pat(cs->cstr_self);
    std::vector<V> fields;
    for (const tt::ClassField* f : cs->cstr_fields) fields.push_back(class_field(f));
    return o::vblock(0, {self, o::vlist(fields), w_.class_sig(cs->cstr_type), w_.ident_map(cs->cstr_meths)});
  }
  V class_field_kind(const tt::ClassFieldKind& k) {
    if (k.is_virtual) return o::vblock(0, {typ(k.cty)});
    return o::vblock(1, {flag(static_cast<long>(k.ovr)), expr(k.exp)});
  }
  V class_field(const tt::ClassField* f) {
    V l = loc(f->cf_loc);
    V desc = class_field_desc(f->cf_desc);
    V attrs = attributes(f->cf_attributes);
    return o::vblock(0, {desc, l, attrs});
  }
  V class_field_desc(const tt::ClassFieldDesc* d) {
    using K = tt::ClassFieldDesc::Kind;
    auto pairs = [&](const Slice<std::pair<std::string_view, Ident::t>>& l) {
      return w_.list(l, [&](const std::pair<std::string_view, Ident::t>& p) {
        return o::vblock(0, {w_.str(p.first), w_.ident(p.second)});
      });
    };
    switch (d->kind) {
      case K::Tcf_inherit: {
        auto* x = tt::as<tt::Tcf_inherit>(d);
        V ce = class_expr(x->ce);
        return o::vblock(0, {flag(static_cast<long>(x->ovr)), ce, w_.opt_str(x->as), pairs(x->vals), pairs(x->meths)});
      }
      case K::Tcf_val: {
        auto* x = tt::as<tt::Tcf_val>(d);
        V k = class_field_kind(x->kind_);
        return o::vblock(1, {str_loc(x->name), w_.mutable_flag(x->mut), w_.ident(x->id), k, w_.b(x->inherited)});
      }
      case K::Tcf_method: {
        auto* x = tt::as<tt::Tcf_method>(d);
        V k = class_field_kind(x->kind_);
        return o::vblock(2, {str_loc(x->name), w_.private_flag(x->priv), k});
      }
      case K::Tcf_constraint: {
        auto* x = tt::as<tt::Tcf_constraint>(d);
        V t2 = typ(x->t2);
        V t1 = typ(x->t1);
        return o::vblock(3, {t1, t2});
      }
      case K::Tcf_initializer: return o::vblock(4, {expr(tt::as<tt::Tcf_initializer>(d)->exp)});
      case K::Tcf_attribute: return o::vblock(5, {attribute(tt::as<tt::Tcf_attribute>(d)->attr)});
    }
    return w_.i(0);
  }
  V class_type(const tt::ClassType* c) {
    V l = loc(c->cltyp_loc);
    V en = env(c->cltyp_env);
    V desc = class_type_desc(c->cltyp_desc);
    V attrs = attributes(c->cltyp_attributes);
    return o::vblock(0, {desc, w_.class_type(c->cltyp_type), en, l, attrs});
  }
  V class_type_desc(const tt::ClassTypeDesc* d) {
    using K = tt::ClassTypeDesc::Kind;
    switch (d->kind) {
      case K::Tcty_constr: {
        auto* x = tt::as<tt::Tcty_constr>(d);
        V args = typs(x->args);
        return o::vblock(0, {w_.path(x->path), lid_loc(x->lid), args});
      }
      case K::Tcty_signature: return o::vblock(1, {class_signature(tt::as<tt::Tcty_signature>(d)->sig)});
      case K::Tcty_arrow: {
        auto* x = tt::as<tt::Tcty_arrow>(d);
        V ct = class_type(x->cty);
        V arg = typ(x->arg);
        return o::vblock(2, {w_.arg_label(x->label), arg, ct});
      }
      case K::Tcty_open: {
        auto* x = tt::as<tt::Tcty_open>(d);
        V ct = class_type(x->cty);
        V od = open_description(x->od);
        return o::vblock(3, {od, ct});
      }
    }
    return w_.i(0);
  }
  V class_signature(const tt::TClassSignature* s) {
    V self = typ(s->csig_self);
    std::vector<V> fields;
    for (const tt::ClassTypeField* f : s->csig_fields) fields.push_back(class_type_field(f));
    return o::vblock(0, {self, o::vlist(fields), w_.class_sig(s->csig_type)});
  }
  V class_type_field(const tt::ClassTypeField* f) {
    V l = loc(f->ctf_loc);
    V desc = class_type_field_desc(f->ctf_desc);
    V attrs = attributes(f->ctf_attributes);
    return o::vblock(0, {desc, l, attrs});
  }
  V class_type_field_desc(const tt::ClassTypeFieldDesc* d) {
    using K = tt::ClassTypeFieldDesc::Kind;
    switch (d->kind) {
      case K::Tctf_inherit: return o::vblock(0, {class_type(tt::as<tt::Tctf_inherit>(d)->cty)});
      case K::Tctf_val: {
        auto* x = tt::as<tt::Tctf_val>(d);
        V t = typ(x->ty);
        return o::vblock(1, {o::vblock(0, {w_.str(x->name), w_.mutable_flag(x->mut), w_.virtual_flag(x->virt), t})});
      }
      case K::Tctf_method: {
        auto* x = tt::as<tt::Tctf_method>(d);
        V t = typ(x->ty);
        return o::vblock(2, {o::vblock(0, {w_.str(x->name), w_.private_flag(x->priv), w_.virtual_flag(x->virt), t})});
      }
      case K::Tctf_constraint: {
        auto* x = tt::as<tt::Tctf_constraint>(d);
        V t2 = typ(x->t2);
        V t1 = typ(x->t1);
        return o::vblock(3, {o::vblock(0, {t1, t2})});
      }
      case K::Tctf_attribute: return o::vblock(4, {attribute(tt::as<tt::Tctf_attribute>(d)->attr)});
    }
    return w_.i(0);
  }

  // the value written for an item declaration node
  V decl(const void* node) const {
    auto it = decls_.find(node);
    if (it == decls_.end()) throw std::logic_error("Cmt_format: an item declaration the tree writer did not write");
    return it->second;
  }

 private:
  static bool is_position(const OValue* x) {
    return x->kind == OValue::Kind::Block && x->tag == 0 && x->fields.size() == 4 &&
           x->fields[0]->kind == OValue::Kind::String && x->fields[1]->kind == OValue::Kind::Int &&
           x->fields[2]->kind == OValue::Kind::Int && x->fields[3]->kind == OValue::Kind::Int;
  }
  static Position position_of(const OValue* x) {
    if (x->pos) return *x->pos;
    return Position{x->fields[0]->s, x->fields[1]->i, x->fields[2]->i, x->fields[3]->i};
  }
  // a boxed integer (custom block): the constant's box, one per literal
  V boxed(const tt::Constant& c, int bytes) {
    auto make_box = [&]() -> V {
      std::string raw;
      auto be32 = [&](std::uint32_t x) {
        for (int s = 3; s >= 0; --s) raw.push_back(static_cast<char>((x >> (8 * s)) & 0xff));
      };
      auto be64 = [&](std::uint64_t x) {
        for (int s = 7; s >= 0; --s) raw.push_back(static_cast<char>((x >> (8 * s)) & 0xff));
      };
      raw.push_back(static_cast<char>(0x19));  // CODE_CUSTOM_FIXED
      if (bytes == 4) {
        raw += "_i";
        raw.push_back('\0');
        be32(static_cast<std::uint32_t>(c.boxed));
        return o::vcustom2(raw, 4, 4);
      }
      if (bytes == 8) {
        raw += "_j";
        raw.push_back('\0');
        be64(static_cast<std::uint64_t>(c.boxed));
        return o::vcustom2(raw, 8, 8);
      }
      raw += "_n";
      raw.push_back('\0');
      if (c.boxed >= INT32_MIN && c.boxed <= INT32_MAX) {
        raw.push_back(1);
        be32(static_cast<std::uint32_t>(c.boxed));
      } else {
        raw.push_back(2);
        be64(static_cast<std::uint64_t>(c.boxed));
      }
      return o::vcustom2(raw, 4, 8);
    };
    if (!c.box) return make_box();
    if (auto it = memo_.find(c.box); it != memo_.end()) return it->second;
    V v = make_box();
    memo_[c.box] = v;
    return v;
  }

  Writer& w_;
  EventWriter& ew_;
  env::t last_env_ = nullptr;
  V last_v_;
  V empty_tbl_;
  std::unordered_map<const void*, V> memo_;
  std::unordered_map<const void*, V> decls_;
  std::map<std::pair<int, int>, V> var_inj_;
};

// ---- index_declarations: Tast_iterator's walk, calling item_declaration --------
class DeclIndexer {
 public:
  DeclIndexer(Writer& w, TreeWriter& tw, UidTbl& tbl) : w_(w), tw_(tw), tbl_(tbl) {}

  void structure(const tt::Structure* s) {
    for (const tt::StructureItem* it : s->str_items) structure_item(it);
  }
  void signature(const tt::Signature* s) {
    for (const tt::SignatureItem* it : s->sig_items) signature_item(it);
  }

 private:
  // item_declaration: a fresh `K node` block for each call, except that a
  // value binding's uids share the one passed in
  void item(int tag, const void* node, const Uid& uid) { tbl_.add(w_.uid(uid), o::vblock(tag, {tw_.decl(node)})); }
  enum Tag {
    Value, Primitive, Value_binding, Type, Constructor, Extension_constructor, Label, Module, Module_substitution,
    Module_binding, Module_type, Class, Class_type
  };

  void structure_item(const tt::StructureItem* it) {
    using K = tt::StructureItemDesc::Kind;
    const tt::StructureItemDesc* d = it->str_desc;
    switch (d->kind) {
      case K::Tstr_eval: expr(tt::as<tt::Tstr_eval>(d)->exp); break;
      case K::Tstr_value:
        for (const tt::ValueBinding* vb : tt::as<tt::Tstr_value>(d)->vbs) value_binding(vb);
        break;
      case K::Tstr_primitive: primitive_description(tt::as<tt::Tstr_primitive>(d)->pd); break;
      case K::Tstr_type:
        for (const tt::TTypeDeclaration* td : tt::as<tt::Tstr_type>(d)->decls) type_declaration(td);
        break;
      case K::Tstr_typext: type_extension(tt::as<tt::Tstr_typext>(d)->ext); break;
      case K::Tstr_exception: extension_constructor(tt::as<tt::Tstr_exception>(d)->exn->tyexn_constructor); break;
      case K::Tstr_module: module_binding(tt::as<tt::Tstr_module>(d)->mb); break;
      case K::Tstr_recmodule:
        for (const tt::ModuleBinding* mb : tt::as<tt::Tstr_recmodule>(d)->mbs) module_binding(mb);
        break;
      case K::Tstr_modtype: module_type_declaration(tt::as<tt::Tstr_modtype>(d)->mtd); break;
      case K::Tstr_class:
        for (const tt::ClassDeclarationItem& c : tt::as<tt::Tstr_class>(d)->classes) {
          item(Class, c.decl, c.decl->ci_decl->cty_uid);
          class_infos(c.decl);
          class_expr(c.decl->ci_expr);
        }
        break;
      case K::Tstr_class_type:
        for (const tt::ClassTypeDeclarationItem& c : tt::as<tt::Tstr_class_type>(d)->classes) class_type_decl(c.decl);
        break;
      case K::Tstr_include: module_expr(tt::as<tt::Tstr_include>(d)->incl->incl_mod); break;
      case K::Tstr_open: module_expr(tt::as<tt::Tstr_open>(d)->od->open_expr); break;
      case K::Tstr_attribute: break;
    }
  }
  void signature_item(const tt::SignatureItem* it) {
    using K = tt::SignatureItemDesc::Kind;
    const tt::SignatureItemDesc* d = it->sig_desc;
    switch (d->kind) {
      case K::Tsig_value: {
        auto* vd = tt::as<tt::Tsig_value>(d)->vd;
        item(Value, vd, vd->val_val->val_uid);
        typ(vd->val_desc);
        break;
      }
      case K::Tsig_primitive: primitive_description(tt::as<tt::Tsig_primitive>(d)->pd); break;
      case K::Tsig_type:
        for (const tt::TTypeDeclaration* td : tt::as<tt::Tsig_type>(d)->decls) type_declaration(td);
        break;
      case K::Tsig_typesubst:
        for (const tt::TTypeDeclaration* td : tt::as<tt::Tsig_typesubst>(d)->decls) type_declaration(td);
        break;
      case K::Tsig_typext: type_extension(tt::as<tt::Tsig_typext>(d)->ext); break;
      case K::Tsig_exception: extension_constructor(tt::as<tt::Tsig_exception>(d)->exn->tyexn_constructor); break;
      case K::Tsig_module: module_declaration(tt::as<tt::Tsig_module>(d)->md); break;
      case K::Tsig_modsubst: {
        auto* ms = tt::as<tt::Tsig_modsubst>(d)->ms;
        item(Module_substitution, ms, ms->ms_uid);
        break;
      }
      case K::Tsig_recmodule:
        for (const tt::TModuleDeclaration* md : tt::as<tt::Tsig_recmodule>(d)->mds) module_declaration(md);
        break;
      case K::Tsig_modtype: module_type_declaration(tt::as<tt::Tsig_modtype>(d)->mtd); break;
      case K::Tsig_modtypesubst: module_type_declaration(tt::as<tt::Tsig_modtypesubst>(d)->mtd); break;
      case K::Tsig_include: module_type(tt::as<tt::Tsig_include>(d)->incl->incl_mod); break;
      case K::Tsig_class:
        for (const tt::TClassDescription* c : tt::as<tt::Tsig_class>(d)->classes) class_type_decl(c);
        break;
      case K::Tsig_class_type:
        for (const tt::TClassTypeDeclaration* c : tt::as<tt::Tsig_class_type>(d)->classes) class_type_decl(c);
        break;
      case K::Tsig_open: case K::Tsig_attribute: break;
    }
  }
  void class_type_decl(const tt::TClassTypeDeclaration* c) {
    item(Class_type, c, c->ci_decl->cty_uid);
    class_infos(c);
    class_type(c->ci_expr);
  }
  template <class A>
  void class_infos(const tt::ClassInfos<A>* ci) {
    for (const tt::TypeParam& p : ci->ci_params) typ(p.ty);
  }
  void value_binding(const tt::ValueBinding* vb) {
    // iter_on_declaration (Value_binding vb): the pattern's bound idents'
    // uids, one decl value
    V d = o::vblock(Value_binding, {tw_.decl(vb)});
    for (const tt::BoundIdent& b : tt::let_bound_idents_full(slice({vb}))) tbl_.add(w_.uid(b.uid), d);
    pat(vb->vb_pat);
    expr(vb->vb_expr);
  }
  void primitive_description(const tt::TPrimitiveDescription* pd) {
    item(Primitive, pd, pd->prim_val->val_uid);
    if (pd->prim_kind.kind == tt::PrimitiveKind::Kind::Tprim_decl) typ(pd->prim_kind.cty);
    else if (pd->prim_kind.cty) typ(pd->prim_kind.cty);
  }
  void label_decl(const tt::TLabelDeclaration* ld) {
    item(Label, ld, ld->ld_uid);
    typ(ld->ld_type);
  }
  void constructor_args(const tt::TConstructorArguments& a) {
    if (!a.is_record) {
      for (const tt::CoreType* c : a.tuple) typ(c);
    } else {
      for (const tt::TLabelDeclaration* ld : a.record) label_decl(ld);
    }
  }
  void type_declaration(const tt::TTypeDeclaration* td) {
    if (!btype::is_row_name(ident::name(td->typ_id))) item(Type, td, td->typ_type->type_uid);
    for (const tt::TypeConstraintItem& c : td->typ_constraints) {
      typ(c.t1);
      typ(c.t2);
    }
    if (td->typ_kind.kind == tt::TTypeKind::Kind::Ttype_variant) {
      for (const tt::TConstructorDeclaration* cd : td->typ_kind.constructors) {
        item(Constructor, cd, cd->cd_uid);
        constructor_args(cd->cd_args);
        if (cd->cd_res) typ(cd->cd_res);
      }
    } else if (td->typ_kind.kind == tt::TTypeKind::Kind::Ttype_record) {
      for (const tt::TLabelDeclaration* ld : td->typ_kind.labels) label_decl(ld);
    }
    if (td->typ_manifest) typ(td->typ_manifest);
    for (const tt::TypeParam& p : td->typ_params) typ(p.ty);
  }
  void type_extension(const tt::TTypeExtension* te) {
    for (const tt::TypeParam& p : te->tyext_params) typ(p.ty);
    for (const tt::TExtensionConstructor* ec : te->tyext_constructors) extension_constructor(ec);
  }
  void extension_constructor(const tt::TExtensionConstructor* ec) {
    item(Extension_constructor, ec, ec->ext_type->ext_uid);
    if (ec->ext_kind.kind == tt::TExtensionConstructorKind::Kind::Text_decl) {
      constructor_args(ec->ext_kind.args);
      if (ec->ext_kind.res) typ(ec->ext_kind.res);
    }
  }
  void module_binding(const tt::ModuleBinding* mb) {
    item(Module_binding, mb, mb->mb_uid);
    module_expr(mb->mb_expr);
  }
  void module_declaration(const tt::TModuleDeclaration* md) {
    item(Module, md, md->md_uid);
    module_type(md->md_type);
  }
  void module_type_declaration(const tt::TModuleTypeDeclaration* mtd) {
    item(Module_type, mtd, mtd->mtd_uid);
    if (mtd->mtd_type) module_type(mtd->mtd_type);
  }
  void functor_parameter(const tt::FunctorParameter& p) {
    if (!p.is_unit) module_type(p.mty);
  }
  void module_expr(const tt::ModuleExpr* m) {
    using K = tt::ModuleExprDesc::Kind;
    const tt::ModuleExprDesc* d = m->mod_desc;
    switch (d->kind) {
      case K::Tmod_ident: break;
      case K::Tmod_structure: structure(tt::as<tt::Tmod_structure>(d)->str); break;
      case K::Tmod_functor: {
        auto* x = tt::as<tt::Tmod_functor>(d);
        functor_parameter(x->param);
        module_expr(x->body);
        break;
      }
      case K::Tmod_apply: {
        auto* x = tt::as<tt::Tmod_apply>(d);
        module_expr(x->fn);
        module_expr(x->arg);
        break;
      }
      case K::Tmod_apply_unit: module_expr(tt::as<tt::Tmod_apply_unit>(d)->fn); break;
      case K::Tmod_constraint: {
        auto* x = tt::as<tt::Tmod_constraint>(d);
        module_expr(x->me);
        if (x->explicit_mty) module_type(x->explicit_mty);
        break;
      }
      case K::Tmod_unpack: expr(tt::as<tt::Tmod_unpack>(d)->exp); break;
    }
  }
  void module_type(const tt::ModuleType* m) {
    using K = tt::ModuleTypeDesc::Kind;
    const tt::ModuleTypeDesc* d = m->mty_desc;
    switch (d->kind) {
      case K::Tmty_ident: case K::Tmty_alias: break;
      case K::Tmty_signature: signature(tt::as<tt::Tmty_signature>(d)->sig); break;
      case K::Tmty_functor: {
        auto* x = tt::as<tt::Tmty_functor>(d);
        functor_parameter(x->param);
        module_type(x->body);
        break;
      }
      case K::Tmty_with: {
        auto* x = tt::as<tt::Tmty_with>(d);
        module_type(x->mty);
        for (const tt::WithConstraintItem& c : x->cstrs) {
          using WK = tt::WithConstraint::Kind;
          switch (c.cstr.kind) {
            case WK::Twith_type: case WK::Twith_typesubst: type_declaration(c.cstr.decl); break;
            case WK::Twith_modtype: case WK::Twith_modtypesubst: module_type(c.cstr.mty); break;
            default: break;
          }
        }
        break;
      }
      case K::Tmty_typeof: module_expr(tt::as<tt::Tmty_typeof>(d)->me); break;
    }
  }
  void pat_extras(const tt::Pattern* p) {
    for (const tt::PatExtraItem& x : p->pat_extra) {
      if (x.extra.kind == tt::PatExtra::Kind::Tpat_unpack && x.extra.pack) package_type(x.extra.pack);
      else if (x.extra.kind == tt::PatExtra::Kind::Tpat_constraint) typ(x.extra.cty);
    }
  }
  void pat(const tt::Pattern* p) {
    pat_extras(p);
    using K = tt::PatternDesc::Kind;
    const tt::PatternDesc* d = p->pat_desc;
    switch (d->kind) {
      case K::Tpat_tuple:
        for (const tt::LabeledPattern& lp : tt::as<tt::Tpat_tuple>(d)->pats) pat(lp.pat);
        break;
      case K::Tpat_construct: {
        auto* x = tt::as<tt::Tpat_construct>(d);
        for (const tt::Pattern* q : x->args) pat(q);
        if (x->annot) typ(x->annot->cty);
        break;
      }
      case K::Tpat_variant:
        if (auto* q = tt::as<tt::Tpat_variant>(d)->arg) pat(q);
        break;
      case K::Tpat_record:
        for (const tt::RecordPatField& f : tt::as<tt::Tpat_record>(d)->fields) pat(f.pat);
        break;
      case K::Tpat_array:
        for (const tt::Pattern* q : tt::as<tt::Tpat_array>(d)->pats) pat(q);
        break;
      case K::Tpat_alias: pat(tt::as<tt::Tpat_alias>(d)->pat); break;
      case K::Tpat_lazy: pat(tt::as<tt::Tpat_lazy>(d)->pat); break;
      case K::Tpat_value: pat(tt::as<tt::Tpat_value>(d)->pat); break;
      case K::Tpat_exception: pat(tt::as<tt::Tpat_exception>(d)->pat); break;
      case K::Tpat_or: {
        auto* x = tt::as<tt::Tpat_or>(d);
        pat(x->p1);
        pat(x->p2);
        break;
      }
      default: break;
    }
  }
  void exp_extra(const tt::ExpExtra& e) {
    using K = tt::ExpExtra::Kind;
    switch (e.kind) {
      case K::Texp_constraint: typ(e.cty); break;
      case K::Texp_coerce:
        if (e.from) typ(e.from);
        typ(e.cty);
        break;
      case K::Texp_poly:
        if (e.cty) typ(e.cty);
        break;
      case K::Texp_newtype: break;
    }
  }
  void case_(const tt::Case* c) {
    pat(c->c_lhs);
    if (c->c_guard) expr(c->c_guard);
    expr(c->c_rhs);
  }
  void binding_op(const tt::BindingOp* b) { expr(b->bop_exp); }
  void expr(const tt::Expression* e) {
    for (const tt::ExpExtraItem& x : e->exp_extra) exp_extra(x.extra);
    using K = tt::ExpressionDesc::Kind;
    const tt::ExpressionDesc* d = e->exp_desc;
    switch (d->kind) {
      case K::Texp_let: {
        auto* x = tt::as<tt::Texp_let>(d);
        for (const tt::ValueBinding* vb : x->vbs) value_binding(vb);
        expr(x->body);
        break;
      }
      case K::Texp_function: {
        auto* x = tt::as<tt::Texp_function>(d);
        for (const tt::FunctionParam* fp : x->params) {
          pat(fp->fp_kind.pat);
          if (fp->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_optional_default) expr(fp->fp_kind.default_);
        }
        if (x->body->kind == tt::FunctionBody::Kind::Tfunction_body) {
          expr(x->body->body);
        } else {
          for (const tt::Case* c : x->body->cases) case_(c);
          if (x->body->exp_extra) exp_extra(*x->body->exp_extra);
        }
        break;
      }
      case K::Texp_apply: {
        auto* x = tt::as<tt::Texp_apply>(d);
        expr(x->fn);
        for (const tt::LabeledArg& a : x->args)
          if (!a.arg.omitted) expr(a.arg.arg);
        break;
      }
      case K::Texp_match: {
        auto* x = tt::as<tt::Texp_match>(d);
        expr(x->exp);
        for (const tt::Case* c : x->comp_cases) case_(c);
        for (const tt::Case* c : x->eff_cases) case_(c);
        break;
      }
      case K::Texp_try: {
        auto* x = tt::as<tt::Texp_try>(d);
        expr(x->exp);
        for (const tt::Case* c : x->exn_cases) case_(c);
        for (const tt::Case* c : x->eff_cases) case_(c);
        break;
      }
      case K::Texp_tuple:
        for (const tt::LabeledExpression& le : tt::as<tt::Texp_tuple>(d)->el) expr(le.exp);
        break;
      case K::Texp_construct:
        for (const tt::Expression* a : tt::as<tt::Texp_construct>(d)->args) expr(a);
        break;
      case K::Texp_variant:
        if (auto* a = tt::as<tt::Texp_variant>(d)->arg) expr(a);
        break;
      case K::Texp_record: {
        auto* x = tt::as<tt::Texp_record>(d);
        for (const tt::RecordField& f : x->fields)
          if (!f.def.kept) expr(f.def.exp);
        if (x->extended_expression) expr(x->extended_expression);
        break;
      }
      case K::Texp_field: expr(tt::as<tt::Texp_field>(d)->exp); break;
      case K::Texp_setfield: {
        auto* x = tt::as<tt::Texp_setfield>(d);
        expr(x->exp);
        expr(x->value);
        break;
      }
      case K::Texp_atomic_loc: expr(tt::as<tt::Texp_atomic_loc>(d)->exp); break;
      case K::Texp_array:
        for (const tt::Expression* a : tt::as<tt::Texp_array>(d)->el) expr(a);
        break;
      case K::Texp_ifthenelse: {
        auto* x = tt::as<tt::Texp_ifthenelse>(d);
        expr(x->cond);
        expr(x->then_);
        if (x->else_) expr(x->else_);
        break;
      }
      case K::Texp_sequence: {
        auto* x = tt::as<tt::Texp_sequence>(d);
        expr(x->e1);
        expr(x->e2);
        break;
      }
      case K::Texp_while: {
        auto* x = tt::as<tt::Texp_while>(d);
        expr(x->cond);
        expr(x->body);
        break;
      }
      case K::Texp_for: {
        auto* x = tt::as<tt::Texp_for>(d);
        expr(x->lo);
        expr(x->hi);
        expr(x->body);
        break;
      }
      case K::Texp_send: expr(tt::as<tt::Texp_send>(d)->obj); break;
      case K::Texp_setinstvar: expr(tt::as<tt::Texp_setinstvar>(d)->value); break;
      case K::Texp_override:
        for (const tt::OverrideField& f : tt::as<tt::Texp_override>(d)->fields) expr(f.exp);
        break;
      case K::Texp_assert: expr(tt::as<tt::Texp_assert>(d)->exp); break;
      case K::Texp_lazy: expr(tt::as<tt::Texp_lazy>(d)->exp); break;
      case K::Texp_object: class_structure(tt::as<tt::Texp_object>(d)->cs); break;
      case K::Texp_pack: module_expr(tt::as<tt::Texp_pack>(d)->me); break;
      case K::Texp_letop: {
        auto* x = tt::as<tt::Texp_letop>(d);
        binding_op(x->let_);
        for (const tt::BindingOp* b : x->ands) binding_op(b);
        case_(x->body);
        break;
      }
      case K::Texp_struct_item: {
        auto* x = tt::as<tt::Texp_struct_item>(d);
        structure_item(x->item);
        expr(x->body);
        break;
      }
      default: break;
    }
  }
  void package_type(const tt::PackageType* p) {
    for (auto& [l, ct] : p->tpt_constraints) typ(ct);
  }
  void typ(const tt::CoreType* c) {
    using K = tt::CoreTypeDesc::Kind;
    const tt::CoreTypeDesc* d = c->ctyp_desc;
    switch (d->kind) {
      case K::Ttyp_arrow: {
        auto* x = tt::as<tt::Ttyp_arrow>(d);
        typ(x->t1);
        typ(x->t2);
        break;
      }
      case K::Ttyp_tuple:
        for (const tt::LabeledCoreType& lt : tt::as<tt::Ttyp_tuple>(d)->tl) typ(lt.ty);
        break;
      case K::Ttyp_constr:
        for (const tt::CoreType* a : tt::as<tt::Ttyp_constr>(d)->args) typ(a);
        break;
      case K::Ttyp_object:
        for (const tt::ObjectField* f : tt::as<tt::Ttyp_object>(d)->fields) typ(f->of_desc.ty);
        break;
      case K::Ttyp_class:
        for (const tt::CoreType* a : tt::as<tt::Ttyp_class>(d)->args) typ(a);
        break;
      case K::Ttyp_alias: typ(tt::as<tt::Ttyp_alias>(d)->ty); break;
      case K::Ttyp_variant:
        for (const tt::RowField* r : tt::as<tt::Ttyp_variant>(d)->fields) {
          if (r->rf_desc.is_tag) {
            for (const tt::CoreType* a : r->rf_desc.types) typ(a);
          } else {
            typ(r->rf_desc.inherit);
          }
        }
        break;
      case K::Ttyp_poly: typ(tt::as<tt::Ttyp_poly>(d)->ty); break;
      case K::Ttyp_package: package_type(tt::as<tt::Ttyp_package>(d)->pack); break;
      case K::Ttyp_open: typ(tt::as<tt::Ttyp_open>(d)->ty); break;
      case K::Ttyp_functor: {
        auto* x = tt::as<tt::Ttyp_functor>(d);
        package_type(x->pack);
        typ(x->ty);
        break;
      }
      default: break;
    }
  }
  void class_expr(const tt::ClassExpr* c) {
    using K = tt::ClassExprDesc::Kind;
    const tt::ClassExprDesc* d = c->cl_desc;
    switch (d->kind) {
      case K::Tcl_constraint: {
        auto* x = tt::as<tt::Tcl_constraint>(d);
        class_expr(x->ce);
        if (x->cty) class_type(x->cty);
        break;
      }
      case K::Tcl_structure: class_structure(tt::as<tt::Tcl_structure>(d)->cs); break;
      case K::Tcl_fun: {
        auto* x = tt::as<tt::Tcl_fun>(d);
        pat(x->pat);
        for (const tt::IdentExpression& ie : x->args) expr(ie.exp);
        class_expr(x->ce);
        break;
      }
      case K::Tcl_apply: {
        auto* x = tt::as<tt::Tcl_apply>(d);
        class_expr(x->ce);
        for (const tt::LabeledArg& a : x->args)
          if (!a.arg.omitted) expr(a.arg.arg);
        break;
      }
      case K::Tcl_let: {
        auto* x = tt::as<tt::Tcl_let>(d);
        for (const tt::ValueBinding* vb : x->vbs) value_binding(vb);
        for (const tt::IdentExpression& ie : x->vals) expr(ie.exp);
        class_expr(x->ce);
        break;
      }
      case K::Tcl_ident:
        for (const tt::CoreType* a : tt::as<tt::Tcl_ident>(d)->args) typ(a);
        break;
      case K::Tcl_open: class_expr(tt::as<tt::Tcl_open>(d)->ce); break;
    }
  }
  void class_structure(const tt::ClassStructure* cs) {
    pat(cs->cstr_self);
    for (const tt::ClassField* f : cs->cstr_fields) {
      using K = tt::ClassFieldDesc::Kind;
      const tt::ClassFieldDesc* d = f->cf_desc;
      switch (d->kind) {
        case K::Tcf_inherit: class_expr(tt::as<tt::Tcf_inherit>(d)->ce); break;
        case K::Tcf_constraint: {
          auto* x = tt::as<tt::Tcf_constraint>(d);
          typ(x->t1);
          typ(x->t2);
          break;
        }
        case K::Tcf_val: class_field_kind(tt::as<tt::Tcf_val>(d)->kind_); break;
        case K::Tcf_method: class_field_kind(tt::as<tt::Tcf_method>(d)->kind_); break;
        case K::Tcf_initializer: expr(tt::as<tt::Tcf_initializer>(d)->exp); break;
        case K::Tcf_attribute: break;
      }
    }
  }
  void class_field_kind(const tt::ClassFieldKind& k) {
    if (k.is_virtual) typ(k.cty);
    else expr(k.exp);
  }
  void class_type(const tt::ClassType* c) {
    using K = tt::ClassTypeDesc::Kind;
    const tt::ClassTypeDesc* d = c->cltyp_desc;
    switch (d->kind) {
      case K::Tcty_signature: {
        const tt::TClassSignature* s = tt::as<tt::Tcty_signature>(d)->sig;
        typ(s->csig_self);
        for (const tt::ClassTypeField* f : s->csig_fields) {
          using FK = tt::ClassTypeFieldDesc::Kind;
          const tt::ClassTypeFieldDesc* fd = f->ctf_desc;
          switch (fd->kind) {
            case FK::Tctf_inherit: class_type(tt::as<tt::Tctf_inherit>(fd)->cty); break;
            case FK::Tctf_val: typ(tt::as<tt::Tctf_val>(fd)->ty); break;
            case FK::Tctf_method: typ(tt::as<tt::Tctf_method>(fd)->ty); break;
            case FK::Tctf_constraint: {
              auto* x = tt::as<tt::Tctf_constraint>(fd);
              typ(x->t1);
              typ(x->t2);
              break;
            }
            case FK::Tctf_attribute: break;
          }
        }
        break;
      }
      case K::Tcty_constr:
        for (const tt::CoreType* a : tt::as<tt::Tcty_constr>(d)->args) typ(a);
        break;
      case K::Tcty_arrow: {
        auto* x = tt::as<tt::Tcty_arrow>(d);
        typ(x->arg);
        class_type(x->cty);
        break;
      }
      case K::Tcty_open: class_type(tt::as<tt::Tcty_open>(d)->cty); break;
    }
  }

  Writer& w_;
  TreeWriter& tw_;
  UidTbl& tbl_;
};

// ---- the reduced shape -------------------------------------------------------------
class ShapeWriter {
 public:
  explicit ShapeWriter(Writer& w) : w_(w) {}
  V shape(shape::t s) {
    if (auto it = memo_.find(s); it != memo_.end()) return it->second;
    V uid = w_.none();
    if (s->has_uid) uid = w_.some_shared(s->uid_obj, [&] { return w_.uid(s->uid); });
    V desc = desc_(s);
    V v = o::vblock(0, {uid, desc, w_.b(s->approximated)});
    memo_[s] = v;
    return v;
  }

 private:
  V item(const shape::Item& it) {
    return w_.shared_by(it.obj, [&] { return o::vblock(0, {w_.str(it.name), w_.i(static_cast<long>(it.kind))}); });
  }
  V map(const PMapNode<shape::Item, shape::t>* n) {
    if (!n) return w_.i(0);
    return o::vblock(0, {map(n->l), item(n->v), shape(n->d), map(n->r), w_.i(n->h)});
  }
  V desc_(shape::t s) {
    using SK = shape::Shape::Kind;
    switch (s->kind) {
      case SK::Leaf: return w_.i(0);
      case SK::Var: return o::vblock(0, {w_.ident(s->var)});
      case SK::Abs: return o::vblock(1, {w_.ident(s->var), shape(s->t1)});
      case SK::App: return o::vblock(2, {shape(s->t1), shape(s->t2)});
      case SK::Struct: return o::vblock(3, {map(s->map.root())});
      case SK::Pack: return o::vblock(4, {w_.ident(s->var)});
      case SK::Alias: return o::vblock(5, {shape(s->t1)});
      case SK::Proj: return o::vblock(6, {shape(s->t1), item(s->item)});
      case SK::Comp_unit: return o::vblock(7, {w_.str(s->str)});
      case SK::Error: return o::vblock(8, {w_.str(s->str)});
    }
    return w_.i(0);
  }
  Writer& w_;
  std::unordered_map<shape::t, V> memo_;
};

// Location.rewrite_absolute_path (no BUILD_PATH_PREFIX_MAP support: the path)
std::string rewrite_absolute_path(const std::string& p) { return p; }

std::string file_digest(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return blake2::blake128(reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

}  // namespace

void set_comments(std::vector<std::pair<std::string_view, Location>> comments) { g_comments = std::move(comments); }
void set_argv(std::vector<std::string> argv) { g_argv = std::move(argv); }
void set_source_name(std::string_view name) { g_source_name = name; }

void record_declaration_dependency(DepKind k, const Uid& uid1, const Uid& uid2) {
  // if not (Uid.equal uid1 uid2)
  if (uid1.kind == uid2.kind && uid1.comp_unit == uid2.comp_unit && uid1.id == uid2.id && uid1.from == uid2.from) return;
  g_deps.push_back(Dep{k, uid1, uid2});
}

void clear() { g_deps.clear(); }

void save_cmt(const std::string& filename, std::string_view modname, const std::optional<std::string>& sourcefile,
              const BinaryAnnots& annots, env::t initial_env, const cmi_format::CmiInfos* cmi, shape::t shape) {
  if (!clflags::binary_annotations || clflags::print_types) {
    clear();
    return;
  }
  std::string out;
  std::optional<std::string> this_crc;
  if (cmi) {
    auto [bytes, crc] = cmi_format::output_cmi_bytes(*cmi);
    out += bytes;
    this_crc = crc;
  }
  Writer w;
  w.set_current_unit_shared(modname);
  EventWriter ew(w);
  TreeWriter tw(w, ew);
  // cmt_ident_occurrences: -bin-annot-occurrences is not ported (the list is [])
  V annots_v;
  switch (annots.kind) {
    case BinaryAnnots::Kind::Implementation: annots_v = o::vblock(1, {tw.structure(annots.structure)}); break;
    case BinaryAnnots::Kind::Interface: annots_v = o::vblock(2, {tw.signature(annots.signature)}); break;
    case BinaryAnnots::Kind::Packed:
      annots_v = o::vblock(0, {w.signature(annots.packed_sg),
                               w.list(annots.packed_files, [&](const std::string& f) { return w.str(zborrow(f)); })});
      break;
    default: annots_v = o::vblock(3, {o::vblock(0, {})}); break;  // Partial_implementation [||]
  }
  UidTbl tbl;
  DeclIndexer ix(w, tw, tbl);
  if (annots.kind == BinaryAnnots::Kind::Implementation) ix.structure(annots.structure);
  else if (annots.kind == BinaryAnnots::Kind::Interface) ix.signature(annots.signature);
  // the record's fields, right to left
  V shape_v = shape ? w.some(ShapeWriter(w).shape(shape)) : w.none();
  V interface_digest = this_crc ? w.some(o::vstr(*this_crc)) : w.none();
  // cmt_imports: List.sort compare (Env.imports ())
  auto imports = env::imports();
  std::sort(imports.begin(), imports.end());
  std::vector<V> imps;
  for (auto& [name, crc] : imports) {
    // the unit's own name: the one Unit_info string (save_cmi's add_import)
    V n = name == modname ? w.unit_name(modname) : w.str(env::import_name(name));
    imps.push_back(o::vblock(0, {n, crc ? w.some(o::vstr(*crc)) : w.none()}));
  }
  V initial = tw.env(initial_env);
  V source_digest = sourcefile ? w.some(o::vstr(file_digest(*sourcefile))) : w.none();
  // Sys.argv's strings: the source file's is the unit's source name
  // (g_source_name); the others are their own
  std::vector<V> args;
  std::map<std::string, V> arg_strs;
  for (std::size_t k = 0; k < g_argv.size(); ++k) {
    V v;
    if (k > 0 && g_source_name.data() && g_argv[k] == g_source_name) v = w.str(g_source_name);
    else v = w.str(zborrow(k == 0 ? rewrite_absolute_path(g_argv[k]) : g_argv[k]));
    args.push_back(v);
    if (k > 0) arg_strs.try_emplace(g_argv[k], v);
  }
  // Load_path.get_paths (): the include directories are the command line's
  // strings (Clflags.include_dirs), "" the current directory
  std::vector<V> visible, hidden;
  auto [vis, hid] = load_path::get_paths();
  auto dir = [&](const std::string& d) {
    if (auto it = arg_strs.find(d); it != arg_strs.end() && !d.empty()) return it->second;
    return w.str(zborrow(d));
  };
  for (const std::string& d : vis) visible.push_back(dir(d));
  for (const std::string& d : hid) hidden.push_back(dir(d));
  V loadpath = o::vblock(0, {o::vlist(visible), o::vlist(hidden)});
  char cwd[4096];
  std::string builddir = getcwd(cwd, sizeof cwd) ? std::string(cwd) : std::string(".");
  std::vector<V> comments;
  for (auto& [s, l] : g_comments) comments.push_back(o::vblock(0, {w.str(s), w.loc(l)}));
  std::vector<V> deps;
  for (auto it = g_deps.rbegin(); it != g_deps.rend(); ++it)
    deps.push_back(o::vblock(0, {w.i(static_cast<long>(it->k)), w.uid(it->a), w.uid(it->b)}));
  V cmt = o::vblock(0, {
                           w.unit_name(modname),                                     // cmt_modname
                           annots_v,                                                 // cmt_annots
                           o::vlist(deps),                                           // cmt_declaration_dependencies
                           o::vlist(comments),                                       // cmt_comments
                           o::vblock(0, args),                                       // cmt_args
                           sourcefile ? w.some(g_source_name.data() && *sourcefile == g_source_name
                                                   ? w.str(g_source_name)
                                                   : w.str(zborrow(*sourcefile)))
                                      : w.none(),  // cmt_sourcefile
                           o::vstr(rewrite_absolute_path(builddir)),                 // cmt_builddir
                           loadpath,                                                 // cmt_loadpath
                           source_digest,                                            // cmt_source_digest
                           initial,                                                  // cmt_initial_env
                           o::vlist(imps),                                           // cmt_imports
                           interface_digest,                                         // cmt_interface_digest
                           w.b(true),                                                // cmt_use_summaries
                           tbl.value(),                                              // cmt_uid_to_decl
                           shape_v,                                                  // cmt_impl_shape
                           w.i(0),                                                   // cmt_ident_occurrences
                       });
  out += cmt_magic_number;
  std::vector<std::uint8_t> bytes = o::marshal(cmt);
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  // Misc.output_to_file_via_temporary
  std::string tmp = filename + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open " + tmp);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!f) throw std::runtime_error("Cannot write " + tmp);
  }
  if (std::rename(tmp.c_str(), filename.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw std::runtime_error("Cannot rename " + tmp + " to " + filename);
  }
  clear();
}

}  // namespace cppcaml::typing::cmt_format
