// Minimal Hindley-Milner core for the type-checker: a mutable, unifiable
// type_expr with the levels-based generalization scheme (Rémy/OCaml-style).
//
// This is the load-bearing engine the dump-projection let us defer: printtyped
// emits no inferred types, so the typer climbed to ~25% with pure resolution.
// Inference is needed for the genuinely type-directed cases (match exhaustiveness,
// ambiguous field/constructor disambiguation) and, ultimately, real artifacts.
//
// Slice 1: the core (repr/unify/instantiate/generalize), unit-tested in
// isolation.  Wiring into the expression typer comes next.
#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cppcaml::infer {

struct Type;
using TypePtr = std::shared_ptr<Type>;

// Generic (generalized) variables carry this level; ordinary vars carry the
// binding depth at which they were created.
constexpr int GENERIC_LEVEL = 1'000'000'000;

struct Type {
  // Any is a dynamic/unknown type for values we cannot infer (qualified lookups,
  // unresolved record fields, ambiguous constructors): it unifies with anything
  // without clashing or propagating, so it never causes a false rejection.
  // Object is a structural object type `< m1 : t1; m2 : t2 >` (method names in
  // `labels`, types in `args`).  It is produced ONLY in the non-strict passes
  // (signature/value-kinds), so the strict pass never has to reason about it.
  // Variant is an open polymorphic-variant row `[> `A | `B of t]` (tag names in
  // `labels`, per-tag arg types in `args`, presence in `tag_has_arg`).  Like
  // Object, only produced in the non-strict passes.
  enum class Kind { Var, Arrow, Tuple, Constr, Link, Any, Object, Variant };
  Kind kind = Kind::Var;
  int level = 0;             // Var: binding level (GENERIC_LEVEL if generalized)
  int id = 0;                // unique id (occurs-check / debug printing)

  TypePtr dom, cod;          // Arrow
  int arrow_label = 0;       // Arrow: 0 Nolabel, 1 Labelled, 2 Optional
  std::string arrow_lbl;     // Arrow: label name (when Labelled/Optional)
  std::vector<TypePtr> args; // Tuple / Constr / Object / Variant
  std::vector<std::string> labels;  // Object: method names; Variant: tag names
  std::vector<char> tag_has_arg;    // Variant: 1 if the tag carries an argument
  std::string path;          // Constr: type-constructor path (e.g. "int", "list")
  int stamp = 0;             // Constr: identity of a local type decl (0 = none).
                             // Two constrs with distinct non-zero stamps are
                             // distinct types even if their paths match.

  TypePtr link;              // Link: forwarding pointer (union-find)
};

struct TypeError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A fresh inference engine.  Owns the level counter and id allocator so multiple
// type-checks don't interfere.
class Engine {
public:
  int level = 0;
  void enter_level() { ++level; }
  void leave_level() { --level; }

  TypePtr fresh_var();
  TypePtr any();  // the shared dynamic/unknown type
  TypePtr arrow(TypePtr dom, TypePtr cod, int label = 0, std::string lbl = "");
  TypePtr tuple(std::vector<TypePtr> elems);
  TypePtr constr(std::string path, std::vector<TypePtr> args = {}, int stamp = 0);
  TypePtr object_type(std::vector<std::string> names, std::vector<TypePtr> types);
  TypePtr variant_type(std::vector<std::string> tags, std::vector<TypePtr> argtys,
                       std::vector<char> has_arg);

  // Follow Link chains to the representative (path-compressing).
  static TypePtr repr(TypePtr t);

  // Make a and b equal, or throw TypeError on a clash.  Updates levels and runs
  // the occurs-check when binding a variable.
  void unify(const TypePtr& a, const TypePtr& b);

  // Copy a generalized type, replacing GENERIC_LEVEL vars with fresh vars at the
  // current level (shared structure for non-generic parts).
  TypePtr instantiate(const TypePtr& scheme);

  // Generalize: any variable with level > the current level becomes generic.
  void generalize(const TypePtr& t);

  // Undo trail, for branch-local typing (GADT refinement): every binding made
  // while a window is open is recorded and can be rolled back, so one match
  // arm's refinement (`a := float`) cannot leak into the next (`a := int32`).
  // Windows nest; undo_to must be called with the matching mark in LIFO order.
  size_t mark();
  void undo_to(size_t m);

private:
  int next_id_ = 0;
  TypePtr any_;  // singleton Any node
  void occurs_and_lower(const TypePtr& var, const TypePtr& t);

  struct Trail { TypePtr node; Type::Kind kind; TypePtr link; int level; };
  std::vector<Trail> trail_;
  int window_depth_ = 0;
  void note(const TypePtr& n);
  // repr is static (compression must be trailed too): the engine with an open
  // window, if any.  Single-threaded by construction.
  static Engine* trail_owner_;
};

// Render a type in OCaml-ish syntax (for tests/debugging only).
std::string show(const TypePtr& t);

}  // namespace cppcaml::infer
