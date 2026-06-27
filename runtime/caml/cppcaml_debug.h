/**************************************************************************/
/*                                                                        */
/*   c++caml development aid (not part of upstream OCaml).                 */
/*                                                                        */
/**************************************************************************/

/* A bootstrap debugging facility: make a wild block-field read legible.

   The bytecode interpreter's GETFIELD opcodes are where a mis-compiled program
   most often crashes -- a value that should be a block (a record/variant) is
   actually something else (an int, a string whose bytes are read as a pointer,
   an under-applied closure), and `Field(accu, n)` segfaults deep in interp.c
   with no context.

   When the environment variable CPPCAML_FIELDTRACE is set, every block-field
   read is recorded in a small ring buffer, and a SIGSEGV handler dumps the most
   recent reads -- each with the faulting bytecode offset, the FIELD index, and a
   DECODED accumulator (int value, or, for a pointer, its raw bytes shown as ASCII
   so a string-used-as-a-pointer is obvious) -- followed by a bytecode backtrace
   (stack slots that point into the code segment, i.e. return addresses).  Map the
   printed offsets to modules with the linker's LINKMAP (run c++link with
   CPPCAML_LINKMAP=1; see tools/cppcaml-resolve.sh).

   Zero behavioural effect, and no work beyond one predicted-not-taken branch per
   field read, unless CPPCAML_FIELDTRACE is set. */

#ifndef CAML_CPPCAML_DEBUG_H
#define CAML_CPPCAML_DEBUG_H

#ifdef CAML_INTERNALS

#include "mlvalues.h"
#include "fix_code.h"

extern int caml_cppcaml_fieldtrace;

/* Read CPPCAML_FIELDTRACE and, if set, install the SIGSEGV handler.  Idempotent;
   called once when the interpreter first initialises. */
void caml_cppcaml_debug_init(void);

/* Record one block-field read (opcode address, the value being indexed, the
   interpreter stack pointer, the currently-executing closure, and the field
   index).  `env` lets the dump print the offending FUNCTION's entry pc (the
   closure's code pointer), which a bytecode offset alone does not give. */
void caml_cppcaml_field_read(code_t opcode_pc, value accu, value *sp, value env,
                             int field);

#define Cppcaml_field_read(opcode_pc, accu, sp, env, field)                 \
  do { if (caml_cppcaml_fieldtrace)                                         \
         caml_cppcaml_field_read((opcode_pc), (accu), (sp), (env), (field)); } \
  while (0)

/* Record one APPLY/APPTERM: the call-site opcode address, the closure being
   called (`accu`), and the argument count.  The dump derives the callee's entry
   pc (Code_val) so the chain of recently-entered FUNCTIONS is visible -- which
   pins a corrupted closure call (a wild value, or a closure whose env disagrees
   with its code) at the CALL, not at the downstream wild field read. */
void caml_cppcaml_apply(code_t call_pc, value accu, int nargs);

#define Cppcaml_apply(call_pc, accu, nargs)                                 \
  do { if (caml_cppcaml_fieldtrace)                                         \
         caml_cppcaml_apply((call_pc), (accu), (nargs)); }                  \
  while (0)

#endif /* CAML_INTERNALS */
#endif /* CAML_CPPCAML_DEBUG_H */
