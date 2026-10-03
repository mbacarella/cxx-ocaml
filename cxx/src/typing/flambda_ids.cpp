// Port of middle_end/{compilation_unit,linkage_name,variable,symbol}.ml and
// middle_end/flambda/base_types/ (see flambda_ids.hpp).
#include "cppcaml/typing/flambda_ids.hpp"

#include <string>

#include "cppcaml/typing/hashtbl.hpp"
#include "cppcaml/typing/lambda.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing {

using format::Formatter;
using format::fprintf;

// The identifiers outlive the middle end's transient pass zones (which are
// dropped once their pass's output program is copied out: evacuation
// copies terms, never identities): they are in the permanent zone.

// ---- Linkage_name ---------------------------------------------------------
long linkage_name::hash(t s) { return hashtbl::hash_string(std::string(s)); }

// ---- Compilation_unit -----------------------------------------------------
namespace compilation_unit {
namespace {
t g_current = nullptr;
}
t create(Ident::t id, linkage_name::t linkage_name) {
  if (!ident::persistent(id)) misc::fatal_error("Compilation_unit.create with non-persistent Ident.t");
  return permanent_zone().make<CompilationUnit>(id, keep_str(linkage_name),
                                                hashtbl::hash_string(std::string(ident::name(id))));
}
int compare(t a, t b) {
  if (a == b) return 0;
  if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
  int c = ident::name(a->id).compare(ident::name(b->id));
  if (c != 0) return c < 0 ? -1 : 1;
  return linkage_name::Cmp{}(a->linkage_name, b->linkage_name);
}
void print(Formatter& ppf, t c) { fprintf(ppf, "%s", std::string(string_for_printing(c))); }
void set_current(t c) { g_current = c; }
t get_current() { return g_current; }
t get_current_exn() {
  if (!g_current) misc::fatal_error("Compilation_unit.get_current_exn");
  return g_current;
}
bool is_current(t c) {
  if (!g_current) misc::fatal_error("Current compilation unit is not set!");
  return equal(g_current, c);
}
}  // namespace compilation_unit

// ---- Variable -------------------------------------------------------------
namespace variable {
namespace {
long g_previous_name_stamp = -1;
}
t create_with_name_string(std::string_view name, compilation_unit::t cu) {
  if (!cu) cu = compilation_unit::get_current_exn();
  long stamp = ++g_previous_name_stamp;
  return permanent_zone().make<VariableDesc>(cu, keep_str(name), stamp);
}
t create_with_same_name_as_ident(Ident::t id) { return create_with_name_string(ident::name(id)); }
std::string unique_name(t v) { return std::string(v->name) + "_" + std::to_string(v->name_stamp); }
void print(Formatter& ppf, t v) {
  if (compilation_unit::equal(v->compilation_unit, compilation_unit::get_current_exn()))
    fprintf(ppf, "%s/%d", std::string(v->name), v->name_stamp);
  else
    fprintf(ppf, "%a.%s/%d", [&](Formatter& f) { compilation_unit::print(f, v->compilation_unit); },
            std::string(v->name), v->name_stamp);
}
void print_opt(Formatter& ppf, t v) {
  if (!v) fprintf(ppf, "<no var>");
  else print(ppf, v);
}
// Make_set.print: "@[<1>{@[%a@ @]}@]", each element "@ %a"
void print_set(Formatter& ppf, const Set& s) {
  fprintf(ppf, "@[<1>{@[%a@ @]}@]", [&](Formatter& f) {
    s.iter([&](t v) { fprintf(f, "@ %a", [&](Formatter& g) { print(g, v); }); });
  });
}
}  // namespace variable

// ---- Symbol ---------------------------------------------------------------
namespace symbol {
linkage_name::t label(t s) {
  if (s->kind == SymbolDesc::Kind::Linkage) return s->label;
  // the variable's compilation unit for the label: the symbol's might be a
  // pack
  compilation_unit::t cu = variable::get_compilation_unit(s->variable);
  std::string l = std::string(cu->linkage_name) + "__" + variable::unique_name(s->variable);
  return zstr(l);
}
int compare(t a, t b) {
  if (a == b) return 0;
  using K = SymbolDesc::Kind;
  if (a->kind == K::Linkage && b->kind == K::Variable) return 1;
  if (a->kind == K::Variable && b->kind == K::Linkage) return -1;
  if (a->kind == K::Linkage) {
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    // (linkage names are unique across a whole project)
    return linkage_name::Cmp{}(a->label, b->label);
  }
  return variable::compare(a->variable, b->variable);
}
long hash(t s) { return s->kind == SymbolDesc::Kind::Linkage ? s->hash : variable::hash(s->variable); }
t of_global_linkage(compilation_unit::t cu, linkage_name::t label) {
  return permanent_zone().make<SymbolDesc>(SymbolDesc::Kind::Linkage, cu, keep_str(label), linkage_name::hash(label),
                                           nullptr);
}
t of_variable(variable::t v) {
  return permanent_zone().make<SymbolDesc>(SymbolDesc::Kind::Variable, variable::get_compilation_unit(v),
                                           linkage_name::t{}, 0, v);
}
t import_for_pack(compilation_unit::t pack, t s) {
  return permanent_zone().make<SymbolDesc>(s->kind, pack, s->label, s->hash, s->variable);
}
void print(Formatter& ppf, t s) { fprintf(ppf, "%s", std::string(label(s))); }
void print_opt(Formatter& ppf, t s) {
  if (!s) fprintf(ppf, "<no symbol>");
  else print(ppf, s);
}
}  // namespace symbol

// ---- Id_types.UnitId ------------------------------------------------------
namespace unit_id {
namespace {
// Id_types.Id.to_string
std::string id_to_string(t x) {
  if (!x->name) return std::to_string(x->id);
  return std::string(*x->name) + "_" + std::to_string(x->id);
}
}  // namespace
// Format.asprintf "%a.%a" Compilation_unit.print unit Innerid.print id
std::string to_string(t x) { return std::string(compilation_unit::string_for_printing(x->unit)) + "." + id_to_string(x); }
void print(Formatter& ppf, t x) {
  fprintf(ppf, "%a.%a", [&](Formatter& f) { compilation_unit::print(f, x->unit); },
          [&](Formatter& f) { fprintf(f, "%s", id_to_string(x)); });
}
}  // namespace unit_id

namespace set_of_closures_id {
namespace {
long g_counter = 0;
}
t create(compilation_unit::t cu, std::optional<std::string_view> name) {
  if (name) name = keep_str(*name);
  return permanent_zone().make<UnitIdDesc>(++g_counter, name, cu);
}
}  // namespace set_of_closures_id
namespace export_id {
namespace {
long g_counter = 0;
}
t create(compilation_unit::t cu, std::optional<std::string_view> name) {
  if (name) name = keep_str(*name);
  return permanent_zone().make<UnitIdDesc>(++g_counter, name, cu);
}
}  // namespace export_id

// ---- Static_exception, Tag ------------------------------------------------
long static_exception::create() { return lambda::next_raise_count(); }
long tag::create_exn(long t) {
  if (t < 0 || t > 255) misc::fatal_error("Tag.create_exn " + std::to_string(t));
  return t;
}

}  // namespace cppcaml::typing
