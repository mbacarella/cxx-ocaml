/**************************************************************************/
/*                                                                        */
/*   c++caml development aid (not part of upstream OCaml).                 */
/*                                                                        */
/**************************************************************************/

/* See caml/cppcaml_debug.h.  Implements the CPPCAML_FIELDTRACE crash dump for
   wild block-field reads. */

#define CAML_INTERNALS

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "caml/cppcaml_debug.h"
#include "caml/domain.h"
#include "caml/fiber.h"

int caml_cppcaml_fieldtrace = 0;

#define CPPCAML_RING 64  /* power of two */
struct cppcaml_entry { long pc_off; value accu; value *sp; int field; };
static struct cppcaml_entry cppcaml_ring[CPPCAML_RING];
static unsigned cppcaml_pos = 0;
static int cppcaml_installed = 0;

void caml_cppcaml_field_read(code_t opcode_pc, value accu, value *sp, int field)
{
  struct cppcaml_entry *e = &cppcaml_ring[cppcaml_pos & (CPPCAML_RING - 1)];
  e->pc_off = (long)(opcode_pc - caml_start_code);
  e->accu = accu;
  e->sp = sp;
  e->field = field;
  cppcaml_pos++;
}

/* Async-signal-safe output: only write(), no malloc / stdio. */
static void put(const char *s) { if (write(2, s, strlen(s)) < 0) { /* ignore */ } }

static void put_long(long v)
{
  char b[24]; int i = (int)sizeof(b) - 1; b[i--] = 0;
  int neg = v < 0;
  unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
  if (u == 0) b[i--] = '0';
  while (u) { b[i--] = (char)('0' + u % 10); u /= 10; }
  if (neg) b[i--] = '-';
  put(b + i + 1);
}

static void put_hex(unsigned long v)
{
  char b[19]; int i = (int)sizeof(b) - 1; b[i--] = 0;
  if (v == 0) b[i--] = '0';
  while (v) { int d = (int)(v & 0xf); b[i--] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
  b[i--] = 'x'; b[i--] = '0';
  put(b + i + 1);
}

/* Decode the accumulator without dereferencing it (the pointer may be wild):
   an immediate prints its int value; a pointer prints its hex and -- when its
   own 8 bytes are printable ASCII -- that text, which is the tell-tale sign of a
   string's contents being read as a block pointer. */
static void dump_accu(value v)
{
  if (v & 1) { put("int "); put_long((long)(((intnat)v) >> 1)); return; }
  put("ptr "); put_hex((unsigned long)v);
  unsigned char *b = (unsigned char *)&v;
  int printable = 1;
  for (int i = 0; i < 8; i++)
    if (b[i] != 0 && (b[i] < 32 || b[i] > 126)) { printable = 0; break; }
  if (printable) {
    char a[9];
    for (int i = 0; i < 8; i++) a[i] = b[i] ? (char)b[i] : '.';
    a[8] = 0;
    put(" ascii='"); put(a); put("'");
  }
}

static void cppcaml_segv(int sig)
{
  put("\n=== CPPCAML field-read crash dump (most recent first) ===\n");
  for (unsigned k = 0; k < 16 && k < cppcaml_pos; k++) {
    struct cppcaml_entry *e =
      &cppcaml_ring[(cppcaml_pos - 1 - k) & (CPPCAML_RING - 1)];
    put("  getfield pc="); put_long(e->pc_off);
    put(" field="); put_long(e->field);
    put(" accu="); dump_accu(e->accu); put("\n");
  }
  /* A bytecode backtrace from the most recent read's stack pointer: stack slots
     that point into the code segment are saved return addresses. */
  if (cppcaml_pos > 0) {
    struct cppcaml_entry *last =
      &cppcaml_ring[(cppcaml_pos - 1) & (CPPCAML_RING - 1)];
    caml_domain_state *d = Caml_state;
    value *sp = last->sp;
    if (sp && d && d->current_stack) {
      value *hi = Stack_high(d->current_stack);
      int n = 0;
      put("--- bytecode backtrace (caller return pcs) ---\n");
      for (value *s = sp; s < hi && n < 24; s++) {
        value w = *s;
        if (w >= (value)caml_start_code &&
            w < (value)caml_start_code + caml_code_size && (w & 3) == 0) {
          put("  retpc "); put_long((long)(((code_t)w) - caml_start_code)); put("\n");
          n++;
        }
      }
    }
  }
  put("=== map pc/retpc offsets to modules: c++link CPPCAML_LINKMAP=1 "
      "(tools/cppcaml-resolve.sh) ===\n");
  signal(sig, SIG_DFL);
  raise(sig);
}

void caml_cppcaml_debug_init(void)
{
  if (cppcaml_installed) return;
  cppcaml_installed = 1;
  caml_cppcaml_fieldtrace = getenv("CPPCAML_FIELDTRACE") != NULL;
  if (caml_cppcaml_fieldtrace) {
    memset(cppcaml_ring, 0, sizeof cppcaml_ring);
    signal(SIGSEGV, cppcaml_segv);
  }
}
