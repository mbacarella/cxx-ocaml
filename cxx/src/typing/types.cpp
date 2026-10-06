#include <unordered_map>
#include <sys/mman.h>
// Port of typing/types.ml.  See types.hpp.
#include "cppcaml/typing/types.hpp"
#include "cppcaml/typing/cmi_image.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace cppcaml::typing {

// zone.hpp's current zone lives here.  The default one is made on first
// use: other translation units' static initializers allocate in it, in
// whatever order the linker runs them.
static Zone& default_zone() {
  static Zone z;
  return z;
}
static Zone* g_zone = nullptr;  // (constant-initialized) nullptr: the default zone
Zone& zone() { return g_zone ? *g_zone : default_zone(); }
Zone& permanent_zone() { return default_zone(); }
void set_zone(Zone* z) { g_zone = z; }
Zone* g_types_zone = nullptr;
ZoneScope::ZoneScope(Zone& z) : saved(g_zone) { g_zone = &z; }
ZoneScope::~ZoneScope() { g_zone = saved; }
const void* fresh_identity() { return zone().alloc(1, 1); }
// never destroyed: the zones' SliceTailNotes erase from it as they die, the
// default zone's at exit -- after a destroyed map, statics dying in the
// reverse of their construction order
std::unordered_map<const void*, SliceTail>& slice_tails() {
  static auto* m = new std::unordered_map<const void*, SliceTail>;
  return *m;
}
SliceTailNote::~SliceTailNote() { slice_tails().erase(p); }
std::vector<const Zone*>& transient_zones() {
  static std::vector<const Zone*> v;
  return v;
}
std::string_view keep_str(std::string_view s) {
  if (!s.data()) return s;
  for (const Zone* z : transient_zones())
    if (z->owns(s.data())) return permanent_zone().str(s);
  return s;
}

static std::unordered_map<std::uint64_t, std::unordered_map<std::size_t, const std::string_view*>>& fname_handles() {
  static std::unordered_map<std::uint64_t, std::unordered_map<std::size_t, const std::string_view*>> h;
  return h;
}
const std::string_view* Fname::intern(std::string_view s) {
  if (!s.data()) return nullptr;
  // a .cmi image being recorded holds its own handles (its positions are
  // read by other processes); the others are interned, permanent
  if (zone().is_fixed()) return zone().make<std::string_view>(s);
  auto& handles = fname_handles();
  auto& byp = handles[reinterpret_cast<std::uintptr_t>(s.data())];
  auto [it, fresh] = byp.try_emplace(s.size(), nullptr);
  if (fresh) it->second = permanent_zone().make<std::string_view>(s);
  return it->second;
}

char* Zone::huge_block(std::size_t sz, bool advise) {
  constexpr std::size_t huge = 2 << 20;
  // over-map by one huge page and trim to a 2 MiB-aligned [sz] span
  void* m = ::mmap(nullptr, sz + huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return nullptr;
  char* base = static_cast<char*>(m);
  char* p = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(base) + huge - 1) & ~(huge - 1));
  if (p > base) ::munmap(base, p - base);
  char* end = base + sz + huge;
  if (end > p + sz) ::munmap(p + sz, end - (p + sz));
#ifdef MADV_HUGEPAGE
  if (advise) ::madvise(p, sz, MADV_HUGEPAGE);
#else
  (void)advise;  // (no transparent huge pages: macOS)
#endif
  return p;
}
void Fname::relocate(const Zone& dying, std::string_view (*copy_of)(void*, std::string_view), void* ctx) {
  auto& handles = fname_handles();
  std::vector<std::pair<std::uint64_t, std::size_t>> moved;
  for (auto& [ptr, bysize] : handles)
    for (auto& [size, h] : bysize)
      if (dying.owns(h->data())) moved.emplace_back(ptr, size);
  for (auto [ptr, size] : moved) {
    auto& bysize = handles[ptr];
    const std::string_view* h = bysize.at(size);
    bysize.erase(size);
    if (bysize.empty()) handles.erase(ptr);
    std::string_view copy = copy_of(ctx, *h);
    *const_cast<std::string_view*>(h) = copy;
    handles[reinterpret_cast<std::uintptr_t>(copy.data())][size] = h;
  }
}

void Zone::drop_protected() {
  for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) it->second(it->first);
  dtors_.clear();
  for (auto& b : blocks_) {
    char* p = b.release();
    // (the pages given back, the range kept: a later use still faults)
    ::madvise(p, ranges_.at(p), MADV_DONTNEED);
    ::mprotect(p, ranges_.at(p), PROT_NONE);
  }
  blocks_.clear();
  ranges_.clear();
  cur_ = nullptr;
  cap_ = off_ = 0;
}
void Zone::Free::operator()(char* p) const {
  if (huge) ::munmap(p, huge);
  else std::free(p);
}

// the OCaml unit a port file belongs to: its basename, a split of a large
// module (typecore_exp.cpp, ctype_unify.cpp ...) counting as that module
static std::string ocaml_unit_of_file(std::string_view file) {
  std::string_view b = file.substr(file.find_last_of('/') + 1);
  b = b.substr(0, b.find('.'));
  for (std::string_view m : {"typecore", "ctype", "typemod"})
    if (b.substr(0, m.size()) == m) return std::string(m);
  if (b == "typedecl_ext") return "typedecl";
  return std::string(b);
}

std::string_view ocaml_literal(const char* unit, std::string_view s) {
  static std::map<std::pair<std::string, std::string>, std::string_view> interned;
  auto key = std::make_pair(ocaml_unit_of_file(unit), std::string(s));
  auto it = interned.find(key);
  if (it != interned.end()) return it->second;
  ZoneScope perm(permanent_zone());
  return interned[key] = zone().str(s);
}

static bool same_contents(const Position& a, const Position& b) {
  return a.pos_fname.data() == b.pos_fname.data() && a.pos_fname.size() == b.pos_fname.size() &&
         a.pos_lnum == b.pos_lnum && a.pos_bol == b.pos_bol && a.pos_cnum == b.pos_cnum;
}
bool same_record(const Position& x) { return x.obj && same_contents(x, *x.obj); }
// the fingerprint of what same_record compares but the offsets: the file
// names (by handle: one per string identity), lines, line starts, the
// positions' identities, loc_ghost
static std::uint64_t loc_fingerprint(const Location& l) {
  std::uint64_t h = 0x9e3779b97f4a7c15ull;
  auto mix = [&](std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
  };
  for (const Position* p : {&l.loc_start, &l.loc_end}) {
    mix(reinterpret_cast<std::uintptr_t>(p->pos_fname.handle()));
    mix(static_cast<std::uint32_t>(p->pos_lnum) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(p->pos_bol)) << 32);
    mix(reinterpret_cast<std::uintptr_t>(p->obj));
  }
  mix(l.loc_ghost);
  return h;
}
const LocRecord* loc_record(const Location& l) {
  return make<LocRecord>(LocRecord{l.loc_start.pos_cnum, l.loc_end.pos_cnum, loc_fingerprint(l)});
}
bool same_record(const Location& x) {
  return x.obj && x.loc_start.pos_cnum == x.obj->start_cnum && x.loc_end.pos_cnum == x.obj->end_cnum &&
         loc_fingerprint(x) == x.obj->fp;
}

std::string_view zborrow(std::string_view s) {
  if (s.data() && (permanent_zone().owns(s.data()) || zone().owns(s.data())))
    return s;
  return zone().str(s);
}

Location location::distinct_record(Location l) {
  l.obj = loc_record(l);
  l.distinct = true;
  return l;
}

std::uint64_t SomeToken::get() const {
  static std::uint64_t next = 0;
  if (!v) v = ++next;
  return v;
}

Location location::none() {
  Position p{"_none_", 0, 0, -1};  // Lexing.dummy_pos with pos_fname "_none_"
  return Location{p, p, true};
}

const Location* LocPtr::share(const Location& l) {
  Zone& z = zone();
  if (!z.scratch && z.owns(reinterpret_cast<const char*>(&l))) return &l;
  if (z.scratch) {
    ZoneScope perm(permanent_zone());
    return make<Location>(l);
  }
  return make<Location>(l);
}
const Location& LocPtr::none_record() {
  static const Location n = [] {
    ZoneScope perm(permanent_zone());
    return location::none();
  }();
  return n;
}

namespace uid {
static long g_id = -1, g_id_param = -1;
// a fresh record's identity
static const void* fresh_obj() { return zone().alloc(1, 1); }
void reinit() { g_id = -1; g_id_param = -1; }
// shape.ml's "" for a uid made with no current unit: one static literal
// (ocamlopt, which built the reference ocamlc.opt, merges the unit's equal
// string constants, so mk's and mk_local_opaque's are the same object)
static std::string_view no_unit_name() {
  static std::string_view s = [] {
    ZoneScope perm(permanent_zone());
    return zone().str("");
  }();
  return s;
}

// Unit_info.modname: the unit's one name string, which every uid of the
// unit carries (and Cmt_format's cmt_modname)
std::string_view unit_name_string(std::string_view modname) {
  static std::string last;
  static std::string_view z;
  if (!z.data() || last != modname) {
    last = std::string(modname);
    ZoneScope perm(permanent_zone());
    z = zone().str(modname);
  }
  return z;
}

Uid mk(const UnitInfo* current_unit) {
  Uid u;
  u.kind = Uid::Kind::Item;
  if (current_unit) {
    u.comp_unit = unit_name_string(current_unit->modname);
    u.from = current_unit->kind;
  } else {
    u.comp_unit = no_unit_name();
    u.from = Uid::From::Impl;
  }
  u.id = ++g_id;
  u.obj = fresh_obj();
  return u;
}
Uid mk_local_opaque(const UnitInfo* current_unit) {
  Uid u;
  u.kind = Uid::Kind::Local_opaque_item;
  u.comp_unit = current_unit ? unit_name_string(current_unit->modname) : no_unit_name();
  u.id = ++g_id_param;
  u.obj = fresh_obj();
  return u;
}
Uid of_compilation_unit_id(std::string_view name) {
  Uid u;
  u.kind = Uid::Kind::Compilation_unit;
  u.comp_unit = zborrow(name);  // Compilation_unit (Ident.name id): the ident's own string
  u.obj = fresh_obj();
  return u;
}
Uid of_predef_id(std::string_view name) {
  Uid u;
  u.kind = Uid::Kind::Predef;
  u.comp_unit = zborrow(name);  // Predef (Ident.name id): the ident's name
  u.obj = fresh_obj();
  return u;
}
bool for_actual_declaration(const Uid& u) { return u.kind == Uid::Kind::Item; }
bool equal(const Uid& a, const Uid& b) {
  return a.kind == b.kind && a.comp_unit == b.comp_unit && a.id == b.id && a.from == b.from;
}
}  // namespace uid

namespace ident {
extern void (*unscoped_change_log)(const Unscoped::Change&);
}

// ---- Variance ------------------------------------------------------------
namespace variance {
long single(F f) {
  switch (f) {
    case F::May_pos: return 1;
    case F::May_neg: return 2 + 4;
    case F::May_weak: return 4;
    case F::Inj: return 8;
    case F::Pos: return 16 + 8 + 1;
    case F::Neg: return 32 + 8 + 4 + 2;
    case F::Inv: return 63;
  }
  return 0;
}
t full() { return single(F::Inv); }
t covariant() { return single(F::Pos); }
t contravariant() { return single(F::Neg); }
static t swap(F f1, F f2, t v, t v2) {
  return set_if(mem(f2, v), f1, set_if(mem(f1, v), f2, v2));
}
t conjugate(t v) {
  t v2 = inter(v, union_(single(F::Inj), single(F::May_weak)));
  return swap(F::Pos, F::Neg, v, swap(F::May_pos, F::May_neg, v, v2));
}
t compose(t v1, t v2) {
  if (mem(F::Inv, v1) && mem(F::Inj, v2)) return full();
  bool mp = (mem(F::May_pos, v1) && mem(F::May_pos, v2)) ||
            (mem(F::May_neg, v1) && mem(F::May_neg, v2));
  bool mn = (mem(F::May_pos, v1) && mem(F::May_neg, v2)) ||
            (mem(F::May_neg, v1) && mem(F::May_pos, v2));
  bool mw = (mem(F::May_weak, v1) && v2 != null) || (v1 != null && mem(F::May_weak, v2));
  bool inj = mem(F::Inj, v1) && mem(F::Inj, v2);
  bool pos = (mem(F::Pos, v1) && mem(F::Pos, v2)) || (mem(F::Neg, v1) && mem(F::Neg, v2));
  bool neg = (mem(F::Pos, v1) && mem(F::Neg, v2)) || (mem(F::Neg, v1) && mem(F::Pos, v2));
  t v = null;
  v = set_if(mp, F::May_pos, v);
  v = set_if(mn, F::May_neg, v);
  v = set_if(mw, F::May_weak, v);
  v = set_if(inj, F::Inj, v);
  v = set_if(pos, F::Pos, v);
  v = set_if(neg, F::Neg, v);
  return v;
}
t strengthen(t v) {
  if (mem(F::May_neg, v)) return v;
  return v & (full() - single(F::May_weak));
}
std::vector<t> unknown_signature(bool injective, long arity) {
  t v = injective ? set(F::Inj, unknown) : unknown;
  return std::vector<t>(static_cast<std::size_t>(arity), v);
}
}  // namespace variance

namespace types {

// ---- singletons ------------------------------------------------------------
// In the pinned region (a fixed address): the objects a .cmi decodes to
// point at them, and cmi_image.hpp maps such objects back into later
// processes unrelocated.
// All in one record at the start of the region, so that each has the same
// address in every process (allocated on first use, whatever static
// initializer asks first).
namespace {
struct Singletons {
  Tnil tnil{{DescKind::Tnil}};
  Commutable cok{Commutable::Kind::Cok, nullptr};
  Commutable cunknown{Commutable::Kind::Cunknown, nullptr};
  FieldKind fkprivate{FieldKind::Kind::FKprivate, nullptr};
  FieldKind fkpublic{FieldKind::Kind::FKpublic, nullptr};
  FieldKind fkabsent{FieldKind::Kind::FKabsent, nullptr};
  RowField rfabsent{RowField::Kind::RFabsent};
  RowField rfnone{RowField::Kind::RFnone};
  AbbrevMemo mnil{AbbrevMemo::Kind::Mnil};
};
Singletons& singletons() {
  static Singletons* const p = cmi_image::pinned_new<Singletons>();
  return *p;
}
}  // namespace

const TypeDesc* tnil() { return &singletons().tnil; }
Commutable* cok() { return &singletons().cok; }
Commutable* cunknown() { return &singletons().cunknown; }
FieldKind* fkprivate() { return &singletons().fkprivate; }
FieldKind* fkpublic() { return &singletons().fkpublic; }
FieldKind* fkabsent() { return &singletons().fkabsent; }
const RowField* rfabsent() { return &singletons().rfabsent; }
const RowField* rfnone() { return &singletons().rfnone; }
const AbbrevMemo* mnil() { return &singletons().mnil; }

// ---- desc constructors ------------------------------------------------------
const TypeDesc* tvar(OptStr name) { return make<Tvar>(TypeDesc{DescKind::Tvar}, name); }
const TypeDesc* tvar_none_literal(const char* unit) {
  static std::map<std::string, const TypeDesc*> lits;
  std::string u = ocaml_unit_of_file(unit);
  auto it = lits.find(u);
  if (it != lits.end()) return it->second;
  ZoneScope perm(permanent_zone());
  return lits[u] = tvar(OptStr::none());
}
const TypeDesc* tarrow(ArgLabel l, TypeExpr* a, TypeExpr* b, Commutable* c) {
  return make<Tarrow>(TypeDesc{DescKind::Tarrow}, l, a, b, c);
}
const TypeDesc* ttuple(Slice<LabeledTy> l) {
  return make<Ttuple>(TypeDesc{DescKind::Ttuple}, l);
}
const TypeDesc* tconstr(Path::t p, Slice<TypeExpr*> args, MemoRef* memo) {
  return make<Tconstr>(TypeDesc{DescKind::Tconstr}, p, args, memo);
}
const TypeDesc* tobject(TypeExpr* f, NameRef* nm) {
  return make<Tobject>(TypeDesc{DescKind::Tobject}, f, nm);
}
const TypeDesc* tfield(std::string_view l, FieldKind* k, TypeExpr* a, TypeExpr* b) {
  return make<Tfield>(TypeDesc{DescKind::Tfield}, l, k, a, b);
}
const TypeDesc* tvariant(const RowDesc* row) {
  return make<Tvariant>(TypeDesc{DescKind::Tvariant}, row);
}
const TypeDesc* tunivar(OptStr name) {
  return make<Tunivar>(TypeDesc{DescKind::Tunivar}, name);
}
const TypeDesc* tpoly(TypeExpr* a, Slice<TypeExpr*> vars) {
  return make<Tpoly>(TypeDesc{DescKind::Tpoly}, a, vars);
}
const TypeDesc* tpackage(const Package* p) {
  return make<Tpackage>(TypeDesc{DescKind::Tpackage}, p);
}
const TypeDesc* tfunctor(ArgLabel l, ident::Unscoped* id, const Package* p, TypeExpr* a) {
  return make<Tfunctor>(TypeDesc{DescKind::Tfunctor}, l, id, p, a);
}
const TypeDesc* tlink(TypeExpr* a) { return make<Tlink>(TypeDesc{DescKind::Tlink}, a); }
const TypeDesc* tsubst(TypeExpr* a, TypeExpr* row) {
  return make<Tsubst>(TypeDesc{DescKind::Tsubst}, a, row);
}

// ---- trail (types.ml "Definitions for backtracking") ----------------------
struct Change {
  enum class Kind : std::uint8_t {
    Ctype, Ccompress, Clevel, Cscope, Cname, Crow, Ckind, Ccommu, Cuniv, Cuident
  };
  Kind kind;
  TypeExpr* ty = nullptr;           // Ctype / Ccompress / Clevel / Cscope
  union {                           // one per kind (the trail is long: keep it small)
    const TypeDesc* desc = nullptr;  // Ctype / Ccompress (old)
    long n;                          // Clevel / Cscope
    NameRef* name;                   // Cname
    RowFieldCell* row;               // Crow
    FieldKind* kind_;                // Ckind
    Commutable* commu;               // Ccommu
    TyOptRef* univ;                  // Cuniv
  };
  union {
    const TypeDesc* desc2 = nullptr;  // Ccompress (new)
    const PathArgs* name_old;         // Cname
    TypeExpr* univ_old;               // Cuniv
  };
  ident::Unscoped::Change uident{};  // Cuident
};

struct Changes;  // Change of change * changes ref | Unchanged | Invalid
struct ChangesRef {
  const Changes* contents;
};
struct Changes {
  enum class Kind : std::uint8_t { Change, Unchanged, Invalid };
  Kind kind;
  const Change* ch = nullptr;
  ChangesRef* next = nullptr;
};
static const Changes g_unchanged{Changes::Kind::Unchanged};
static const Changes g_invalid{Changes::Kind::Invalid};

// The trail's entries live in their own zone: OCaml's GC reclaims those no
// snapshot reaches; here, when the last live snapshot dies and the trail has
// grown, its zone is dropped and the trail starts afresh (nothing can reach
// the old entries: only the trail and the snapshots point into the zone).
static std::unique_ptr<Zone> g_trail_zone = std::make_unique<Zone>();
static std::size_t g_trail_entries = 0;

// `trail = Local_store.s_table ref Unchanged`: a ref holding the current ref.
static ChangesRef* g_trail = g_trail_zone->make<ChangesRef>(&g_unchanged);

static void log_change(const Change& ch) {
  // no live snapshot: no backtrack nor undo_compress can reach this entry
  if (Snapshot::live() == 0) return;
  ChangesRef* r2 = g_trail_zone->make<ChangesRef>(&g_unchanged);
  const Change* c = g_trail_zone->make<Change>(ch);
  g_trail->contents = g_trail_zone->make<Changes>(Changes::Kind::Change, c, r2);
  g_trail = r2;
  ++g_trail_entries;
}

Snapshot::~Snapshot() {
  if (--live() == 0 && g_trail_entries > 4096) {
    g_trail_zone = std::make_unique<Zone>();
    g_trail = g_trail_zone->make<ChangesRef>(&g_unchanged);
    g_trail_entries = 0;
  }
}

static void log_uident(const ident::Unscoped::Change& c) {
  Change ch{Change::Kind::Cuident};
  ch.uident = c;
  log_change(ch);
}
static const bool g_uident_installed =
    (ident::unscoped_change_log = &log_uident, true);

// ---- field_kind ------------------------------------------------------------
FieldKind* field_kind_internal_repr(FieldKind* fk) {
  while (fk->kind == FieldKind::Kind::FKvar &&
         fk->field_kind->kind != FieldKind::Kind::FKprivate)
    fk = fk->field_kind;
  return fk;
}
FieldKindView field_kind_repr(FieldKind* fk) {
  switch (field_kind_internal_repr(fk)->kind) {
    case FieldKind::Kind::FKvar: return FieldKindView::Fprivate;
    case FieldKind::Kind::FKpublic: return FieldKindView::Fpublic;
    default: return FieldKindView::Fabsent;
  }
}
FieldKind* field_public() { return fkpublic(); }
FieldKind* field_absent() { return fkabsent(); }
FieldKind* field_private() {
  return make<FieldKind>(FieldKind::Kind::FKvar, fkprivate());
}

// ---- commutable --------------------------------------------------------------
bool is_commu_ok(const Commutable* c) {
  while (c->kind == Commutable::Kind::Cvar) c = c->commu;
  return c->kind == Commutable::Kind::Cok;
}
Commutable* commu_ok() { return cok(); }
Commutable* commu_var() { return make<Commutable>(Commutable::Kind::Cvar, cunknown()); }

// ---- representative ----------------------------------------------------------
static bool absent_field(const TypeDesc* d) {
  auto* f = as<Tfield>(d);
  return f && field_kind_internal_repr(f->kind_)->kind == FieldKind::Kind::FKabsent;
}

// repr_link t d t': follow the links from t', then make [t] point at the
// last node through the last link's own desc [d] (logged, Ccompress)
static TypeExpr* repr_link(TypeExpr* t, const TypeDesc* d, TypeExpr* t2) {
  for (;;) {
    const TypeDesc* d2 = t2->desc;
    if (auto* l = as<Tlink>(d2)) { d = d2; t2 = l->ty; continue; }
    if (absent_field(d2)) { d = d2; t2 = as<Tfield>(d2)->rest; continue; }
    Change ch{Change::Kind::Ccompress};
    ch.ty = t;
    ch.desc = t->desc;
    ch.desc2 = d;
    log_change(ch);
    t->desc = d;
    return t2;
  }
}

static TypeExpr* repr_link1(TypeExpr* t, TypeExpr* t2) {
  const TypeDesc* d2 = t2->desc;
  if (auto* l = as<Tlink>(d2)) return repr_link(t, d2, l->ty);
  if (absent_field(d2)) return repr_link(t, d2, as<Tfield>(d2)->rest);
  return t2;
}

TypeExpr* repr_slow(TypeExpr* t) {
  const TypeDesc* d = t->desc;
  if (auto* l = as<Tlink>(d)) return repr_link1(t, l->ty);
  if (absent_field(d)) return repr_link1(t, as<Tfield>(d)->rest);
  return t;
}


// ---- marks -------------------------------------------------------------------
struct TypeMark {
  bool is_hash;
  long mark = 0;
  std::vector<TypeExpr*>* marked = nullptr;  // the mark's list of marked nodes (reused)
  std::unordered_set<TypeExpr*> visited;
};

// type_marks = all the bits in marks_mask (Sys.int_size - 27 = 36 of them),
// kept as a stack: the head of types.ml's available_marks list is last.
// with_type_mark takes the head and gives it back on exit, so the uses nest.
static std::vector<long> g_available_marks = [] {
  std::vector<long> v;
  for (int x = 63 - 27 - 1; x >= 0; --x) v.push_back(1L << (x + 27));
  return v;
}();
// one list of marked nodes per mark bit, emptied on release (its buffer stays)
static std::vector<TypeExpr*> g_marked[64];

void with_type_mark(FnRef<void(TypeMark&)> f) {
  if (!g_available_marks.empty()) {
    long m = g_available_marks.back();
    g_available_marks.pop_back();
    std::vector<TypeExpr*>& lst = g_marked[__builtin_ctzl(static_cast<unsigned long>(m))];
    lst.clear();
    TypeMark mk{false, m, &lst};
    struct Restore {
      long m;
      std::vector<TypeExpr*>& lst;
      ~Restore() {
        for (TypeExpr* ty : lst) ty->scope &= ~m;
        lst.clear();
        g_available_marks.push_back(m);
      }
    } restore{m, lst};
    f(mk);
  } else {
    TypeMark mk{true};
    f(mk);
  }
}

bool not_marked_node(TypeMark& mark, TypeExpr* t) {
  if (!mark.is_hash) return (repr(t)->scope & mark.mark) == 0;
  return !mark.visited.count(repr(t));
}

static bool try_mark_transient(TypeMark& mark, TypeExpr* ty) {
  if (!mark.is_hash) {
    if (ty->scope & mark.mark) return false;
    ty->scope |= mark.mark;
    mark.marked->push_back(ty);
    return true;
  }
  return mark.visited.insert(ty).second;
}
bool try_mark_node(TypeMark& mark, TypeExpr* t) { return try_mark_transient(mark, repr(t)); }

// ---- Transient_expr ------------------------------------------------------------
namespace transient_expr {
TypeExpr* create(const TypeDesc* desc, long level, long scope, long id) {
  return make<TypeExpr>(desc, level, scope, id);
}
void set_desc(TypeExpr* ty, const TypeDesc* d) { ty->desc = d; }
void set_stub_desc(TypeExpr* ty, const TypeDesc* d) {
  auto* v = as<Tvar>(ty->desc);
  if (!v || v->name.some) throw std::logic_error("Types.Transient_expr.set_stub_desc");
  ty->desc = d;
}
void set_level(TypeExpr* ty, long lv) { ty->level = lv; }
long get_scope(TypeExpr* ty) { return ty->scope & scope_mask; }
long get_marks(TypeExpr* ty) { return static_cast<long>(static_cast<unsigned long>(ty->scope) >> 27); }
void set_scope(TypeExpr* ty, long sc) {
  if (sc & marks_mask) throw std::invalid_argument("Types.Transient_expr.set_scope");
  ty->scope = (ty->scope & marks_mask) | sc;
}
}  // namespace transient_expr

int compare_type(TypeExpr* a, TypeExpr* b) {
  long x = get_id(a), y = get_id(b);
  return x < y ? -1 : x > y ? 1 : 0;
}

// ---- rows ----------------------------------------------------------------------
const RowDesc* create_row(Slice<RowFieldEntry> fields, TypeExpr* more, bool closed,
                          const FixedExplanation* fixed, const PathArgs* name) {
  return make<RowDesc>(fields, more, closed, fixed, name);
}

Slice<RowFieldEntry> row_fields(const RowDesc* row, Zone* in) {
  auto* v = as<Tvariant>(get_desc(row->row_more));
  if (!v) return row->row_fields;
  Slice<RowFieldEntry> rest = row_fields(v->row, in);
  if (row->row_fields.empty()) return rest;  // [] @ l == l
  std::vector<RowFieldEntry> out(row->row_fields.begin(), row->row_fields.end());
  out.insert(out.end(), rest.begin(), rest.end());
  Zone& z = in ? *in : zone();
  Slice<RowFieldEntry> r = slice_in(z, out);
  note_slice_tail(z, r, rest);  // `@` shares its second list
  return r;
}

static const RowDesc* row_repr_no_fields(const RowDesc* row) {
  for (;;) {
    auto* v = as<Tvariant>(get_desc(row->row_more));
    if (!v) return row;
    row = v->row;
  }
}

TypeExpr* row_more(const RowDesc* row) { return row_repr_no_fields(row)->row_more; }
bool row_closed(const RowDesc* row) { return row_repr_no_fields(row)->row_closed; }
const FixedExplanation* row_fixed(const RowDesc* row) {
  return row_repr_no_fields(row)->row_fixed;
}
const PathArgs* row_name(const RowDesc* row) { return row_repr_no_fields(row)->row_name; }

const RowField* get_row_field(std::string_view tag, const RowDesc* row) {
  for (;;) {
    for (auto& e : row->row_fields)
      if (e.label == tag) return e.field;
    auto* v = as<Tvariant>(get_desc(row->row_more));
    if (!v) return rfabsent();
    row = v->row;
  }
}

const RowDesc* set_row_name(const RowDesc* row, const PathArgs* name) {
  auto fields = row_fields(row);
  const RowDesc* r = row_repr_no_fields(row);
  return make<RowDesc>(slice(fields), r->row_more, r->row_closed, r->row_fixed, name);
}

const RowDesc* subst_row_name_path(
    const std::vector<std::pair<Ident::t, Path::t>>& id_map, const RowDesc* row) {
  const PathArgs* nm = row_name(row);
  if (!nm) return row;
  return set_row_name(row, make<PathArgs>(path::subst(id_map, nm->path), nm->args, nm->tail));
}

RowDescRepr row_repr(const RowDesc* row, Zone* in) {
  auto fields = row_fields(row, in);
  const RowDesc* r = row_repr_no_fields(row);
  return {std::move(fields), r->row_more, r->row_closed, r->row_fixed, r->row_name};
}

// row_field_repr_aux: follow the ext chain, accumulating conjunct lists.
static const RowField* row_field_repr_aux(std::vector<TypeExpr*> tl, const RowField* f) {
  for (;;) {
    switch (f->kind) {
      case RowField::Kind::RFeither: {
        const RowField* nxt = f->ext->contents;
        if (nxt->kind == RowField::Kind::RFnone) {
          std::vector<TypeExpr*> args = tl;
          args.insert(args.end(), f->arg_type.begin(), f->arg_type.end());
          return make<RowField>(RowField::Kind::RFeither, nullptr, f->no_arg, slice(args),
                                f->matched, f->ext);
        }
        tl.insert(tl.end(), f->arg_type.begin(), f->arg_type.end());
        f = nxt;
        continue;
      }
      case RowField::Kind::RFpresent:
        if (f->present && !tl.empty())
          return make<RowField>(RowField::Kind::RFpresent, tl.front());
        return f;
      default:
        return f;
    }
  }
}

RowFieldView row_field_repr(const RowField* fi) {
  const RowField* r = row_field_repr_aux({}, fi);
  RowFieldView v;
  switch (r->kind) {
    case RowField::Kind::RFeither:
      v.kind = RowFieldView::Kind::Reither;
      v.constant = r->no_arg;
      v.arg_types.assign(r->arg_type.begin(), r->arg_type.end());
      v.matched = r->matched;
      break;
    case RowField::Kind::RFpresent:
      v.kind = RowFieldView::Kind::Rpresent;
      v.present = r->present;
      break;
    default:
      v.kind = RowFieldView::Kind::Rabsent;
  }
  return v;
}

static RowFieldCell* row_field_ext(const RowField* fi) {
  for (;;) {
    if (fi->kind != RowField::Kind::RFeither) throw std::logic_error("Types.row_field_ext ");
    if (fi->ext->contents->kind == RowField::Kind::RFnone) return fi->ext;
    fi = fi->ext->contents;
  }
}

const RowField* rf_present(TypeExpr* oty) {
  return make<RowField>(RowField::Kind::RFpresent, oty);
}
const TypeKind* type_abstract_literal(const char* unit, TypeOrigin::Kind origin) {
  static std::map<std::pair<std::string, int>, const TypeKind*> lits;
  auto key = std::make_pair(ocaml_unit_of_file(unit), static_cast<int>(origin));
  auto it = lits.find(key);
  if (it != lits.end()) return it->second;
  ZoneScope perm(permanent_zone());
  auto* k = make<TypeKind>();
  k->kind = TypeKind::Kind::Type_abstract;
  k->origin.kind = origin;
  return lits[key] = k;
}
const RowField* rf_present_none_literal(const char* unit) {
  static std::map<std::string, const RowField*> lits;
  std::string u = ocaml_unit_of_file(unit);
  auto it = lits.find(u);
  if (it != lits.end()) return it->second;
  ZoneScope perm(permanent_zone());
  return lits[u] = rf_present(nullptr);
}
const RowField* rf_absent() { return rfabsent(); }
const RowField* rf_either(const RowField* use_ext_of, bool no_arg,
                          Slice<TypeExpr*> arg_type, bool matched) {
  RowFieldCell* ext = use_ext_of ? row_field_ext(use_ext_of) : make<RowFieldCell>(rfnone());
  return make<RowField>(RowField::Kind::RFeither, nullptr, no_arg, arg_type, matched, ext);
}
const RowField* rf_either_of(TypeExpr* oty) {
  if (!oty) return rf_either(nullptr, true, {}, false);
  return rf_either(nullptr, false, slice({oty}), false);
}
bool eq_row_field_ext(const RowField* a, const RowField* b) {
  return row_field_ext(a) == row_field_ext(b);
}
bool changed_row_field_exts(const std::vector<const RowField*>& l,
                            FnRef<void()> f) {
  std::vector<RowFieldCell*> exts;
  for (auto* x : l) exts.push_back(row_field_ext(x));
  f();
  for (auto* r : exts)
    if (r->contents->kind != RowField::Kind::RFnone) return true;
  return false;
}

// ---- signature helpers ----------------------------------------------------------
Visibility item_visibility(const SignatureItem* it) { return it->vis; }

std::vector<Ident::t> bound_value_identifiers(Signature sg) {
  std::vector<Ident::t> out;
  using SK = SignatureItem::Kind;
  for (auto* it : sg) {
    switch (it->kind) {
      case SK::Sig_value:
        if (it->value->val_kind.kind == ValueKind::Kind::Val_reg) out.push_back(it->id);
        break;
      case SK::Sig_typext:
        out.push_back(it->id);
        break;
      case SK::Sig_module:
        if (it->presence == ModulePresence::Mp_present) out.push_back(it->id);
        break;
      case SK::Sig_class:
        out.push_back(it->id);
        break;
      default:
        break;
    }
  }
  return out;
}

Ident::t signature_item_id(const SignatureItem* it) { return it->id; }

// ---- type creators -------------------------------------------------------------
static long g_new_id = -1;
long& new_id() { return g_new_id; }

TypeExpr* create_expr(const TypeDesc* desc, long level, long scope, long id) {
  return transient_expr::create(desc, level, scope, id);
}

TypeExpr* proto_newty3(long level, long scope, const TypeDesc* desc) {
  ++g_new_id;
  return create_expr(desc, level, scope, g_new_id);
}

// ---- backtracking utilities ----------------------------------------------------
static void undo_change(const Change& c) {
  switch (c.kind) {
    case Change::Kind::Ctype:
    case Change::Kind::Ccompress:
      transient_expr::set_desc(c.ty, c.desc);
      break;
    case Change::Kind::Clevel:
      transient_expr::set_level(c.ty, c.n);
      break;
    case Change::Kind::Cscope:
      transient_expr::set_scope(c.ty, c.n);
      break;
    case Change::Kind::Cname:
      c.name->contents = c.name_old;
      break;
    case Change::Kind::Crow:
      c.row->contents = rfnone();
      break;
    case Change::Kind::Ckind:
      c.kind_->field_kind = fkprivate();
      break;
    case Change::Kind::Ccommu:
      c.commu->commu = cunknown();
      break;
    case Change::Kind::Cuniv:
      c.univ->contents = c.univ_old;
      break;
    case Change::Kind::Cuident:
      ident::Unscoped::undo_change(c.uident);
      break;
  }
}

static long g_last_snapshot = 0;

static void log_type(TypeExpr* ty) {
  if (ty->id <= g_last_snapshot) {
    Change ch{Change::Kind::Ctype};
    ch.ty = ty;
    ch.desc = ty->desc;
    log_change(ch);
  }
}

void link_type(TypeExpr* ty, TypeExpr* ty2) {
  ty = repr(ty);
  ty2 = repr(ty2);
  if (ty == ty2) return;
  log_type(ty);
  const TypeDesc* desc = ty->desc;
  transient_expr::set_desc(ty, tlink(ty2));
  // Name is a user-supplied name for this unification variable (obtained
  // through a type annotation for instance).
  auto* v1 = as<Tvar>(desc);
  auto* v2 = as<Tvar>(ty2->desc);
  if (v1 && v2) {
    if (v1->name.some && !v2->name.some) {
      log_type(ty2);
      transient_expr::set_desc(ty2, tvar(v1->name));
    } else if (v1->name.some && v2->name.some) {
      if (ty->level < ty2->level) {
        log_type(ty2);
        transient_expr::set_desc(ty2, tvar(v1->name));
      }
    }
  }
}

void set_type_desc(TypeExpr* ty, const TypeDesc* td) {
  ty = repr(ty);
  if (td != ty->desc) {
    log_type(ty);
    transient_expr::set_desc(ty, td);
  }
}

void set_level(TypeExpr* ty, long level) {
  ty = repr(ty);
  if (level != ty->level) {
    if (ty->id <= g_last_snapshot) {
      Change ch{Change::Kind::Clevel};
      ch.ty = ty;
      ch.n = ty->level;
      log_change(ch);
    }
    transient_expr::set_level(ty, level);
  }
}

void set_scope(TypeExpr* ty, long scope) {
  ty = repr(ty);
  long prev_scope = ty->scope & scope_mask;
  if (scope != prev_scope) {
    if (ty->id <= g_last_snapshot) {
      Change ch{Change::Kind::Cscope};
      ch.ty = ty;
      ch.n = prev_scope;
      log_change(ch);
    }
    transient_expr::set_scope(ty, scope);
  }
}

void set_univar(TyOptRef* rty, TypeExpr* ty) {
  Change ch{Change::Kind::Cuniv};
  ch.univ = rty;
  ch.univ_old = rty->contents;
  log_change(ch);
  rty->contents = ty;
}

void set_name(NameRef* nm, const PathArgs* v) {
  Change ch{Change::Kind::Cname};
  ch.name = nm;
  ch.name_old = nm->contents;
  log_change(ch);
  nm->contents = v;
}

void link_row_field_ext(const RowField* inside, const RowField* v) {
  for (;;) {
    if (inside->kind != RowField::Kind::RFeither)
      throw std::invalid_argument("Types.link_row_field_ext");
    RowFieldCell* e = inside->ext;
    if (e->contents->kind == RowField::Kind::RFnone) {
      if (v->kind == RowField::Kind::RFnone)
        throw std::logic_error("Types.link_row_field_ext: RFnone");
      Change ch{Change::Kind::Crow};
      ch.row = e;
      log_change(ch);
      e->contents = v;
      return;
    }
    inside = e->contents;
  }
}

void link_kind(FieldKind* inside, FieldKind* k) {
  for (;;) {
    if (inside->kind != FieldKind::Kind::FKvar) throw std::invalid_argument("Types.link_kind");
    if (inside->field_kind->kind == FieldKind::Kind::FKprivate) {
      // prevent a loop by normalizing k and comparing it with inside
      k = field_kind_internal_repr(k);
      if (k != inside) {
        Change ch{Change::Kind::Ckind};
        ch.kind_ = inside;
        log_change(ch);
        inside->field_kind = k;
      }
      return;
    }
    inside = inside->field_kind;
  }
}

static Commutable* commu_repr(Commutable* c) {
  while (c->kind == Commutable::Kind::Cvar &&
         c->commu->kind != Commutable::Kind::Cunknown)
    c = c->commu;
  return c;
}

void link_commu(Commutable* inside, Commutable* c) {
  for (;;) {
    if (inside->kind != Commutable::Kind::Cvar) throw std::invalid_argument("Types.link_commu");
    if (inside->commu->kind == Commutable::Kind::Cunknown) {
      c = commu_repr(c);
      if (c != inside) {
        Change ch{Change::Kind::Ccommu};
        ch.commu = inside;
        log_change(ch);
        inside->commu = c;
      }
      return;
    }
    inside = inside->commu;
  }
}

void set_commu_ok(Commutable* c) { link_commu(c, cok()); }

Snapshot snapshot() {
  long old = g_last_snapshot;
  g_last_snapshot = g_new_id;
  return {g_trail, old};
}

void backtrack(FnRef<void()> cleanup, Snapshot s) {
  const Changes* c = s.changes->contents;
  switch (c->kind) {
    case Changes::Kind::Unchanged:
      g_last_snapshot = s.old;
      return;
    case Changes::Kind::Invalid:
      throw std::runtime_error("Types.backtrack");
    case Changes::Kind::Change: {
      cleanup();
      // rev_log accumulates oldest-first then the list is walked in that
      // order: `rev_log [] change` builds newest-first, and List.iter undoes
      // newest-first.
      std::vector<const Change*> backlog;  // oldest first
      for (const Changes* x = c;;) {
        if (x->kind == Changes::Kind::Unchanged) break;
        if (x->kind == Changes::Kind::Invalid) throw std::logic_error("Types.rev_log");
        const Changes* d = x->next->contents;
        x->next->contents = &g_invalid;
        backlog.push_back(x->ch);
        x = d;
      }
      for (auto it = backlog.rbegin(); it != backlog.rend(); ++it) undo_change(**it);
      s.changes->contents = &g_unchanged;
      g_last_snapshot = s.old;
      g_trail = s.changes;
      return;
    }
  }
}

void undo_first_change_after(Snapshot s) {
  const Changes* c = s.changes->contents;
  if (c->kind == Changes::Kind::Change) undo_change(*c->ch);
}

void undo_compress(Snapshot s) {
  const Changes* c = s.changes->contents;
  if (c->kind != Changes::Kind::Change) return;
  // rev_compress_log: the refs whose change is a Ccompress, newest first.
  std::vector<ChangesRef*> log;
  for (ChangesRef* r = s.changes;;) {
    const Changes* x = r->contents;
    if (x->kind != Changes::Kind::Change) break;
    if (x->ch->kind == Change::Kind::Ccompress) log.insert(log.begin(), r);
    r = x->next;
  }
  for (ChangesRef* r : log) {
    const Changes* x = r->contents;
    if (x->kind == Changes::Kind::Change && x->ch->kind == Change::Kind::Ccompress &&
        x->ch->ty->desc == x->ch->desc2) {
      transient_expr::set_desc(x->ch->ty, x->ch->desc);
      r->contents = x->next->contents;
    }
  }
}

}  // namespace types

}  // namespace cppcaml::typing
