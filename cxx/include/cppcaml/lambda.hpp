// The Lambda intermediate representation (OCaml's lambda/lambda.mli) and the
// `-dlambda` printer.  Stage after typing: translate the parsetree (with the
// inference results that are load-bearing here -- value kinds, etc.) into Lambda
// and validate byte-for-byte against `ocamlc -dlambda` over the corpus, the same
// dump-parity loop that drove lex/parse/type.
//
// Slice 1: top-level value bindings of constants/simple exprs -> the module's
// (setglobal L<name>! (let (...) (makeblock 0 ...))) form.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cppcaml/ast.hpp"

namespace cppcaml::lambda {

// Value representation kind (lambda/lambda.mli value_kind), as printed by
// -dlambda: Pintval -> "[int]", Pfloatval -> "[float]", Pgenval -> (nothing).
enum class ValueKind { Gen, Int, Float, Boxedint32, Boxedint64, Nativeint };

struct Lam;
using LamPtr = std::shared_ptr<Lam>;

// An identifier with a stamp (normalized by first dump appearance, like the
// typedtree harness).  name "" for compiler temporaries shown as *match*.
struct Ident {
  std::string name;
  int stamp = 0;
  bool temp = false;  // a compiler-generated binder (printed *name*/stamp)
};

// Lambda primitives we emit so far (printlambda spelling in the comment).
enum class Prim {
  Addint,    // +
  Subint,    // -
  Mulint,    // *
  Field,     // field n
  FieldImm,  // field_imm n
  Makeblock, // makeblock tag
  Setglobal, // setglobal id
  Global,    // global id  (as an arg of field)
  NotEqInt,  // !=
  EqInt,     // ==
  Makemutable, // makemutable tag (shape)  (ref / mutable record)
  FieldInt,    // field_int n   (deref of an immediate-contents ref)
  FieldMut,    // field_mut n   (deref of a pointer-contents ref)
  SetfieldImm, // setfield_imm n  (:= into an immediate-contents ref)
  SetfieldPtr, // setfield_ptr n  (:= into a pointer-contents ref)
  Offsetref,   // +:=n   (incr/decr)
  Ccall,       // a C external call, printed by its C name (prim_id)
  IntCmp,      // integer comparison; spelling (< > <= >= == !=) in prim_id
  Raise,       // (raise e)
  Reraise,     // (reraise e)  (exception handler fall-through)
};

struct Lam {
  enum class K { Var, ConstInt, ConstChar, ConstFloat, ConstString, ConstBlock,
                 Apply, Function, Let, Letrec, Prim, IfThenElse, Sequence,
                 Switch, For, While, Try };
  K k;

  Ident var;                       // Var
  long long int_val = 0;           // ConstInt
  std::string str_val;             // ConstFloat (verbatim) / ConstString

  LamPtr fn;                       // Apply
  std::vector<LamPtr> args;        // Apply / Prim

  // Function
  std::vector<std::pair<Ident, ValueKind>> params;
  ValueKind ret_kind = ValueKind::Gen;
  LamPtr body;

  // Let: a group of bindings (kind shown as =[kind]) then a body
  struct Binding { Ident id; ValueKind kind; LamPtr val; };
  std::vector<Binding> bindings;

  // Prim
  Prim prim;
  int prim_arg = 0;                // field index / makeblock tag / offsetref delta
  std::string prim_id;             // global name (e.g. "Stdlib") / Ccall name / cmp op
  std::vector<ValueKind> blk_shape;  // makemutable block_shape (per-field kinds)
  bool downto_ = false;            // For: counts down

  // IfThenElse
  LamPtr cond, then_, else_;

  // Switch (Lswitch): scrutinee in `cond`; integer-constant and block-tag arms;
  // sw_default null => exhaustive (printed "switch*"), else "switch".
  struct SwitchCase { int tag; LamPtr body; };
  std::vector<SwitchCase> sw_consts, sw_blocks;
  LamPtr sw_default;
};

// Translate a structure into the module's Lambda term (the setglobal form).
// `module_name` is the capitalized file basename (e.g. "L0").
LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name);

// Print in -dlambda format (stamps normalized by first appearance).
void print_dlambda(const LamPtr& code, std::ostream& out);

// Render a constant node (ConstInt/Char/Float/String/Block) in printlambda's
// structured_constant syntax -- used by the bytecode `const` instruction printer.
std::string structured_constant(const LamPtr& c);

}  // namespace cppcaml::lambda
