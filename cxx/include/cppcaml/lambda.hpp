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
};

struct Lam {
  enum class K { Var, ConstInt, ConstFloat, ConstString, ConstBlock, Apply,
                 Function, Let, Prim, IfThenElse, Sequence };
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
  int prim_arg = 0;                // field index / makeblock tag
  std::string prim_id;             // global name (e.g. "Stdlib")

  // IfThenElse
  LamPtr cond, then_, else_;
};

// Translate a structure into the module's Lambda term (the setglobal form).
// `module_name` is the capitalized file basename (e.g. "L0").
LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name);

// Print in -dlambda format (stamps normalized by first appearance).
void print_dlambda(const LamPtr& code, std::ostream& out);

}  // namespace cppcaml::lambda
