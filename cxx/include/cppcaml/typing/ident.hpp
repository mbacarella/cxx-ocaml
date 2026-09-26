// Port of typing/ident.ml (TYPECHECKER.md).  An Ident.t is an immutable value;
// here a zone-allocated `Ident` reached through `Ident::t` (a const pointer),
// compared with same/equal, never by pointer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

namespace ident {

inline constexpr int lowest_scope = 0;
inline constexpr int highest_scope = 100'000'000;

// ident.ml `Unscoped`: a mutable cell (Udesc {name; stamp} | Ulink t).
struct Unscoped {
  enum class State : std::uint8_t { Udesc, Ulink };
  State state;
  std::string_view name;  // Udesc
  int stamp = 0;          // Udesc
  Unscoped* ulink = nullptr;  // Ulink

  struct Desc { std::string_view name; int stamp; };
  static Unscoped* create(std::string_view s);
  static Desc get_desc(const Unscoped* us);
  static std::string_view name_of(const Unscoped* us) { return get_desc(us).name; }
  static Unscoped* refresh(const Unscoped* us);
  static bool equal(const Unscoped* a, const Unscoped* b);
  static int stamp_of(const Unscoped* us) { return get_desc(us).stamp; }
  static bool same(const Unscoped* a, const Unscoped* b);
  static Unscoped* repr(Unscoped* us);
  // `change` = (us, old state); logged through Types' trail (Cuident).
  struct Change { Unscoped* us; State state; std::string_view name; int stamp; Unscoped* ulink; };
  static void undo_change(const Change& c);
  static void link(Unscoped* a, Unscoped* b);
};

}  // namespace ident

struct Ident {
  enum class Kind : std::uint8_t { Local, Scoped, Global, Predef, Unscoped };
  Kind kind;
  std::string_view name_;   // all but Unscoped
  int stamp_ = 0;           // Local / Scoped / Predef
  int scope_ = 0;           // Scoped
  ident::Unscoped* us = nullptr;  // Unscoped

  using t = const Ident*;

  static t create_scoped(int scope, std::string_view s);
  static t create_local(std::string_view s);
  static t of_unscoped(ident::Unscoped* u);
  static t create_predef(std::string_view s);
  static t create_persistent(std::string_view s);
  // Rebuild an ident read from a cmi (the stamp is the writer's).
  static t make_raw(Kind k, std::string_view name, int stamp, int scope,
                    ident::Unscoped* us);
};

namespace ident {

using t = Ident::t;

ident::Unscoped* find_unscoped(t id);
std::string_view name(t id);
t rename(t id);
std::string unique_name(t id);
std::string unique_toplevel_name(t id);
bool persistent(t id);
bool equal(t a, t b);
bool same(t a, t b);
int stamp(t id);
int compare_stamp(t a, t b);
int scope(t id);
void reinit();
bool global(t id);
bool is_predef(t id);
bool is_unscoped(t id);
int compare(t a, t b);
int hash(t id);
// Ident.print with Clflags.unique_ids off (the compiler's default).
std::string print(t id);

// The stamp counters (Local_store refs in ident.ml).
int& currentstamp();
int& predefstamp();

}  // namespace ident

}  // namespace cppcaml::typing
