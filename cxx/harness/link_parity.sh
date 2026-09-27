#!/usr/bin/env bash
# Linker / librarian oracle (Bytelink, Symtable, Bytelibrarian, Dll, Ccomp):
# each case links (or archives) the same inputs with ocamlc.opt and with
# c++ocamlc, one after the other in the same directory (-g records it), and
# compares the files produced, stdout, stderr and the exit code byte for
# byte, then runs the executables.  The inputs are compiled by ocamlc.opt
# (c++ocamlc's .cmo are byte-identical anyway); a case with a .c input
# compiles it with each compiler.
#
# The C-toolchain cases (-custom, -output-obj, -output-complete-*,
# -make-runtime, C stubs) need an INSTALLED standard library (caml/*.h,
# libcamlrun.a, stublibs, ld.conf): INST=<its lib/ocaml> (default: the staged
# install /tmp/cxxsw/usr/local/lib/ocaml; `make install DESTDIR=/tmp/cxxsw`
# makes one); they are skipped without it.  Their binaries embed temporary
# file names, so when ocamlc.opt does not reproduce its own output twice
# they are compared on behaviour only (NONDET).
#
# Usage: link_parity.sh [case ...]   (ROOT= the built tree, CPP=, REF=)
#   SAME / DIFF / NONDET-OK / SKIP per case; outputs in /tmp/link_parity
set -u
SELF="$(readlink -f "$0")"
ROOT="${ROOT:-$(cd "$(dirname "$SELF")/../.." && pwd)}"
REF="${REF:-$ROOT/ocamlc.opt}"
CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
INST="${INST:-/tmp/cxxsw/usr/local/lib/ocaml}"
RUN="$ROOT/runtime/ocamlrun"
OUT=/tmp/link_parity
rm -rf "$OUT"; mkdir -p "$OUT"
W=$(mktemp -d)
trap 'rm -rf "${W:?}"' EXIT
ulimit -v 16000000
export OCAMLLIB="$ROOT/stdlib"

results=()

# the sources the cases share
sources() {
  cat > a.ml <<'EOF'
let greet s = Printf.sprintf "hello %s (%d)" s (List.length [1;2;3])
exception Oops of string
let fail () = raise (Oops "a")
EOF
  cat > a.mli <<'EOF'
val greet : string -> string
exception Oops of string
val fail : unit -> unit
EOF
  cat > b.ml <<'EOF'
let () =
  print_endline (A.greet "world");
  (try A.fail () with A.Oops s -> Printf.printf "caught %s\n" s);
  Printf.printf "%s\n" (String.concat "," (List.map string_of_int [1;2;3]))
EOF
  cat > u.ml <<'EOF'
let () =
  Printf.printf "pid>0: %b\n" (Unix.getpid () > 0);
  print_endline (Str.global_replace (Str.regexp "a+") "X" "baaad aa")
EOF
  cat > t.ml <<'EOF'
let () =
  let m = Mutex.create () in
  let r = ref 0 in
  let t = Thread.create (fun () -> Mutex.lock m; incr r; Mutex.unlock m) () in
  Thread.join t;
  Printf.printf "threads %d\n" !r
EOF
  cat > ext.ml <<'EOF'
external my_twice : int -> int = "cppcaml_test_twice"
let () = Printf.printf "twice 21 = %d\n" (my_twice 21)
EOF
  cat > stub.c <<'EOF'
#include <caml/mlvalues.h>
value cppcaml_test_twice(value n) { return Val_long(2 * Long_val(n)); }
EOF
  cat > bad.ml <<'EOF'
external nope : int -> int = "cppcaml_no_such_primitive"
let () = print_int (nope 1)
EOF
}

# both <case> <outputs...> -- <args...>: run both compilers on the args;
# the outputs (files) are moved to $OUT/<case>/<side>/
both() {
  local name="$1"; shift
  local outs=()
  while [ "$1" != "--" ]; do outs+=("$1"); shift; done
  shift
  local side C f
  for side in o c; do
    for f in "${outs[@]}"; do rm -rf "$f"; done
    if [ $side = o ]; then C="$REF"; else C="$CPP"; fi
    mkdir -p "$OUT/$name/$side"
    "$C" "$@" >"$OUT/$name/$side/stdout" 2>"$OUT/$name/$side/stderr"
    echo $? > "$OUT/$name/$side/rc"
    for f in "${outs[@]}"; do [ -e "$f" ] && cp -r "$f" "$OUT/$name/$side/"; done
  done
}

# run_exe <case> <exe> [direct]: run each side's executable (under
# runtime/ocamlrun unless direct), output to <side>/run
run_exe() {
  local name="$1" exe="$2" how="${3:-}" side
  for side in o c; do
    local e="$OUT/$name/$side/$exe"
    [ -e "$e" ] || continue
    if [ "$how" = direct ]; then (cd "$OUT/$name/$side" && timeout 20 "./$exe") >"$OUT/$name/$side/run" 2>&1
    else (cd "$OUT/$name/$side" && timeout 20 "$RUN" "./$exe") >"$OUT/$name/$side/run" 2>&1; fi
    echo "rc=$?" >> "$OUT/$name/$side/run"
  done
}

# verdict <case>: everything under o/ and c/ byte for byte
verdict() {
  local name="$1"
  if diff -r "$OUT/$name/o" "$OUT/$name/c" >/dev/null 2>&1; then results+=("SAME $name")
  else results+=("DIFF $name"); fi
}

# nondet <case> <outputs...> -- <args...>: a C-toolchain case; byte-for-byte
# when ocamlc.opt reproduces itself, else the files' presence, the run
# output, stdout/stderr/rc
nondet() {
  local name="$1"; shift
  local outs=() a
  for a in "$@"; do [ "$a" = "--" ] && break; outs+=("$a"); done
  shift $(( ${#outs[@]} + 1 ))
  both "$name" "${outs[@]}" -- "$@"
  # ocamlc.opt again
  local f
  for f in "${outs[@]}"; do rm -rf "$f"; done
  mkdir -p "$OUT/$name/o2"
  "$REF" "$@" >/dev/null 2>&1
  for f in "${outs[@]}"; do [ -e "$f" ] && cp -r "$f" "$OUT/$name/o2/"; done
  local det=1
  for f in "${outs[@]}"; do
    if [ -e "$OUT/$name/o/$f" ] && ! cmp -s "$OUT/$name/o/$f" "$OUT/$name/o2/$f"; then det=0; fi
  done
  rm -rf "$OUT/$name/o2"
  echo $det > "$OUT/$name/deterministic"
}
verdict_nondet() {
  local name="$1"; shift
  if [ "$(cat "$OUT/$name/deterministic")" = 1 ]; then verdict "$name"; return; fi
  local ok=1 f side
  for f in stdout stderr rc run; do
    [ -e "$OUT/$name/o/$f" ] || continue
    cmp -s "$OUT/$name/o/$f" "$OUT/$name/c/$f" || ok=0
  done
  for f in "$@"; do
    [ -e "$OUT/$name/o/$f" ] && [ ! -e "$OUT/$name/c/$f" ] && ok=0
    [ ! -e "$OUT/$name/o/$f" ] && [ -e "$OUT/$name/c/$f" ] && ok=0
  done
  if [ $ok = 1 ]; then results+=("NONDET-OK $name"); else results+=("DIFF $name"); fi
}

case_dir() { rm -rf "$W/$1"; mkdir -p "$W/$1"; cd "$W/$1" || exit 1; sources; }
compile() { "$REF" "$@" >/dev/null 2>&1; }

# ---- the cases ----------------------------------------------------------------

c_plain() { case_dir plain; compile -c a.mli a.ml b.ml; both plain prog -- a.cmo b.cmo -o prog; run_exe plain prog; verdict plain; }
c_g() { case_dir g; compile -g -c a.mli a.ml b.ml; both g prog -- -g a.cmo b.cmo -o prog; run_exe g prog; verdict g; }
c_g_nog() { case_dir g_nog; compile -g -c a.mli a.ml b.ml; both g_nog prog -- a.cmo b.cmo -o prog; run_exe g_nog prog; verdict g_nog; }
c_linkall() { case_dir linkall; compile -c a.mli a.ml b.ml; both linkall prog -- -linkall a.cmo b.cmo -o prog; run_exe linkall prog; verdict linkall; }
c_hints() { case_dir hints; compile -c a.mli a.ml b.ml; both hints prog -- -bytecode-hints a.cmo b.cmo -o prog; run_exe hints prog; verdict hints; }
c_dllpath() { case_dir dllpath; compile -c a.mli a.ml b.ml; both dllpath prog -- -dllpath /foo -dllpath /bar/baz a.cmo b.cmo -o prog; run_exe dllpath prog; verdict dllpath; }
c_launch_exe() { case_dir launch_exe; compile -c a.mli a.ml b.ml; both launch_exe prog -- -launch-method exe a.cmo b.cmo -o prog; run_exe launch_exe prog; verdict launch_exe; }
c_launch_exe_bindir() { case_dir launch_exe_bindir; compile -c a.mli a.ml b.ml; both launch_exe_bindir prog -- -launch-method "exe /opt/ocaml/bin" a.cmo b.cmo -o prog; run_exe launch_exe_bindir prog; verdict launch_exe_bindir; }
c_launch_sh() { case_dir launch_sh; compile -c a.mli a.ml b.ml; both launch_sh prog -- -launch-method sh -runtime-search enable a.cmo b.cmo -o prog; run_exe launch_sh prog; verdict launch_sh; }
c_launch_binsh() { case_dir launch_binsh; compile -c a.mli a.ml b.ml; both launch_binsh prog -- -launch-method /bin/sh -runtime-search fallback a.cmo b.cmo -o prog; run_exe launch_binsh prog; verdict launch_binsh; }
c_search_fallback_exe() { case_dir search_fallback_exe; compile -c a.mli a.ml b.ml; both search_fallback_exe prog -- -launch-method exe -runtime-search fallback a.cmo b.cmo -o prog; run_exe search_fallback_exe prog; verdict search_fallback_exe; }
c_search_enable_exe() { case_dir search_enable_exe; compile -c a.mli a.ml b.ml; both search_enable_exe prog -- -launch-method exe -runtime-search enable a.cmo b.cmo -o prog; run_exe search_enable_exe prog; verdict search_enable_exe; }
c_use_runtime() { case_dir use_runtime; compile -c a.mli a.ml b.ml; both use_runtime prog -- -use-runtime "$RUN" a.cmo b.cmo -o prog; run_exe use_runtime prog direct; verdict use_runtime; }
c_runtime_variant() { case_dir runtime_variant; compile -c a.mli a.ml b.ml; both runtime_variant prog -- -runtime-variant d a.cmo b.cmo -o prog; run_exe runtime_variant prog; verdict runtime_variant; }
c_without_runtime() { case_dir without_runtime; compile -c a.mli a.ml b.ml; both without_runtime prog -- -without-runtime a.cmo b.cmo -o prog; run_exe without_runtime prog; verdict without_runtime; }
c_stdlib_default() { case_dir stdlib_default; compile -c a.mli a.ml b.ml; both stdlib_default prog -- -set-runtime-default standard_library_default=/some/where a.cmo b.cmo -o prog; run_exe stdlib_default prog; verdict stdlib_default; }
c_unix_str() { case_dir unix_str; compile -I +unix -I +str -c u.ml; both unix_str prog -- -I +unix -I +str unix.cma str.cma u.cmo -o prog; run_exe unix_str prog; verdict unix_str; }
c_unix_str_g() { case_dir unix_str_g; compile -g -I +unix -I +str -c u.ml; both unix_str_g prog -- -g -I +unix -I +str unix.cma str.cma u.cmo -o prog; run_exe unix_str_g prog; verdict unix_str_g; }
c_threads() { case_dir threads; compile -I +unix -I +threads -c t.ml; both threads prog -- -I +unix -I +threads unix.cma threads.cma t.cmo -o prog; run_exe threads prog; verdict threads; }
c_noautolink() { case_dir noautolink; compile -I +unix -I +str -c u.ml; both noautolink prog -- -noautolink -I +unix -I +str unix.cma str.cma u.cmo -o prog; verdict noautolink; }
c_lib() { case_dir lib; compile -c a.mli a.ml b.ml; both lib lib.cma -- -a a.cmo -o lib.cma; verdict lib; }
c_lib_g() { case_dir lib_g; compile -g -c a.mli a.ml b.ml; both lib_g lib.cma -- -a -g a.cmo b.cmo -o lib.cma; verdict lib_g; }
c_lib_linkall() { case_dir lib_linkall; compile -c a.mli a.ml b.ml; both lib_linkall lib.cma -- -a -linkall a.cmo -o lib.cma; verdict lib_linkall; }
c_lib_cstuff() { case_dir lib_cstuff; compile -c a.mli a.ml b.ml; both lib_cstuff lib.cma -- -a -cclib -lfoo -cclib "-lbar -lbaz" -ccopt -O2 -ccopt -DX=1 -dllib -lqux -dllib-suffixed -lquux -dllib dllzzz.so a.cmo -o lib.cma; verdict lib_cstuff; }
c_lib_custom() { case_dir lib_custom; compile -c a.mli a.ml b.ml; both lib_custom lib.cma -- -a -custom -cclib -lfoo a.cmo -o lib.cma; verdict lib_custom; }
c_lib_of_lib() {
  case_dir lib_of_lib; compile -c a.mli a.ml b.ml
  "$REF" -a -cclib -lfoo -dllib -lqux a.cmo -o l1.cma >/dev/null 2>&1
  both lib_of_lib lib.cma -- -a l1.cma b.cmo -o lib.cma; verdict lib_of_lib
}
c_link_lib() {
  case_dir link_lib; compile -c a.mli a.ml b.ml
  "$REF" -a a.cmo -o lib.cma >/dev/null 2>&1
  both link_lib prog -- lib.cma b.cmo -o prog; run_exe link_lib prog; verdict link_lib
}
c_link_lib_unused() {
  case_dir link_lib_unused; compile -c a.mli a.ml b.ml
  echo 'let () = print_endline "hi"' > h.ml; compile -c h.ml
  "$REF" -a a.cmo -o lib.cma >/dev/null 2>&1
  both link_lib_unused prog -- lib.cma h.cmo -o prog; run_exe link_lib_unused prog; verdict link_lib_unused
}
c_link_lib_dllib() {
  # a library naming C stubs that exist (the unix ones, by -dllib)
  case_dir link_lib_dllib; compile -c a.mli a.ml b.ml
  "$REF" -a -dllib -lunixbyt a.cmo -o lib.cma >/dev/null 2>&1
  both link_lib_dllib prog -- -I +unix -dllib-suffixed -lunixbyt lib.cma b.cmo -o prog; run_exe link_lib_dllib prog; verdict link_lib_dllib
}
c_err_missing() { case_dir err_missing; both err_missing prog -- nosuch.cmo -o prog; verdict err_missing; }
c_err_notobj() { case_dir err_notobj; echo junk > junk.cmo; both err_notobj prog -- junk.cmo -o prog; verdict err_notobj; }
c_err_order() { case_dir err_order; compile -c a.mli a.ml b.ml; both err_order prog -- b.cmo a.cmo -o prog; verdict err_order; }
c_err_dup() { case_dir err_dup; compile -c a.mli a.ml b.ml; cp a.cmo a2.cmo; both err_dup prog -- a.cmo a2.cmo b.cmo -o prog; verdict err_dup; }
c_err_noimpl() { case_dir err_noimpl; compile -c a.mli a.ml b.ml; both err_noimpl prog -- b.cmo -o prog; verdict err_noimpl; }
c_err_inconsistent() {
  case_dir err_inconsistent; compile -c a.mli a.ml b.ml
  cp b.cmo b_old.cmo
  echo 'val greet : string -> string exception Oops of string val fail : unit -> unit val extra : int' > a.mli
  echo 'let greet s = s exception Oops of string let fail () = () let extra = 1' > a.ml
  compile -c a.mli a.ml
  both err_inconsistent prog -- a.cmo b_old.cmo -o prog; verdict err_inconsistent
}
c_err_prim() { case_dir err_prim; compile -c bad.ml; both err_prim prog -- bad.cmo -o prog; verdict err_prim; }
c_err_wrongname() { case_dir err_wrongname; compile -c a.mli a.ml b.ml; both err_wrongname b.cmo -- a.cmo b.cmo -o b.cmo; verdict err_wrongname; }
c_err_dll() { case_dir err_dll; compile -c a.mli a.ml b.ml; both err_dll prog -- -dllib -lnosuchstubs a.cmo b.cmo -o prog; verdict err_dll; }
c_err_lib_missing() { case_dir err_lib_missing; both err_lib_missing lib.cma -- -a nosuch.cmo -o lib.cma; verdict err_lib_missing; }

# C toolchain
need_inst() {
  if [ ! -f "$INST/caml/mlvalues.h" ]; then results+=("SKIP $1 (INST=$INST: no installed stdlib)"); return 1; fi
  export OCAMLLIB="$INST"
}
c_custom() {
  need_inst custom || return; case_dir custom; compile -c a.mli a.ml b.ml
  nondet custom prog -- -custom a.cmo b.cmo -o prog; run_exe custom prog direct; verdict_nondet custom prog
  export OCAMLLIB="$ROOT/stdlib"
}
c_custom_stub() {
  need_inst custom_stub || return; case_dir custom_stub; compile -c ext.ml
  nondet custom_stub prog stub.o -- -custom stub.c ext.cmo -o prog; run_exe custom_stub prog direct
  verdict_nondet custom_stub prog stub.o; export OCAMLLIB="$ROOT/stdlib"
}
c_camlprimc() {
  need_inst camlprimc || return; case_dir camlprimc; compile -c a.mli a.ml b.ml
  nondet camlprimc prog prog.camlprim.c -- -custom -dcamlprimc a.cmo b.cmo -o prog; run_exe camlprimc prog direct
  verdict_nondet camlprimc prog prog.camlprim.c; export OCAMLLIB="$ROOT/stdlib"
}
c_output_c() {
  need_inst output_c || return; case_dir output_c; compile -c a.mli a.ml b.ml
  nondet output_c prog.c -- -output-obj a.cmo b.cmo -o prog.c; verdict_nondet output_c prog.c; export OCAMLLIB="$ROOT/stdlib"
}
c_output_obj() {
  need_inst output_obj || return; case_dir output_obj; compile -c a.mli a.ml b.ml
  nondet output_obj prog.o -- -output-obj a.cmo b.cmo -o prog.o; verdict_nondet output_obj prog.o; export OCAMLLIB="$ROOT/stdlib"
}
c_output_complete_obj() {
  need_inst output_complete_obj || return; case_dir output_complete_obj; compile -c a.mli a.ml b.ml
  nondet output_complete_obj prog.o -- -output-complete-obj a.cmo b.cmo -o prog.o; verdict_nondet output_complete_obj prog.o
  export OCAMLLIB="$ROOT/stdlib"
}
c_output_complete_exe() {
  need_inst output_complete_exe || return; case_dir output_complete_exe; compile -c a.mli a.ml b.ml
  nondet output_complete_exe prog -- -output-complete-exe a.cmo b.cmo -o prog; run_exe output_complete_exe prog direct
  verdict_nondet output_complete_exe prog; export OCAMLLIB="$ROOT/stdlib"
}
c_output_complete_exe_stub() {
  need_inst output_complete_exe_stub || return; case_dir output_complete_exe_stub; compile -c ext.ml
  nondet output_complete_exe_stub prog stub.o -- -output-complete-exe stub.c ext.cmo -o prog
  run_exe output_complete_exe_stub prog direct; verdict_nondet output_complete_exe_stub prog stub.o
  export OCAMLLIB="$ROOT/stdlib"
}
c_make_runtime() {
  need_inst make_runtime || return; case_dir make_runtime; compile -c ext.ml
  nondet make_runtime myrt stub.o -- -make-runtime stub.c -o myrt; verdict_nondet make_runtime myrt stub.o
  export OCAMLLIB="$ROOT/stdlib"
}
c_custom_unix() {
  need_inst custom_unix || return; case_dir custom_unix; compile -I +unix -I +str -c u.ml
  nondet custom_unix prog -- -custom -I +unix -I +str unix.cma str.cma u.cmo -o prog; run_exe custom_unix prog direct
  verdict_nondet custom_unix prog; export OCAMLLIB="$ROOT/stdlib"
}
c_cfile_only() {
  need_inst cfile_only || return; case_dir cfile_only
  nondet cfile_only stub.o -- -c stub.c; verdict_nondet cfile_only stub.o; export OCAMLLIB="$ROOT/stdlib"
}
c_cfile_o() {
  need_inst cfile_o || return; case_dir cfile_o
  nondet cfile_o mystub.o -- -c stub.c -o mystub.o; verdict_nondet cfile_o mystub.o; export OCAMLLIB="$ROOT/stdlib"
}
c_lib_stub_dll() {
  # a library with a stub DLL built by hand, linked (not custom): the Dll check
  need_inst lib_stub_dll || return; case_dir lib_stub_dll; compile -c ext.ml
  cc -shared -fPIC -I "$INST" -o dllmystub.so stub.c
  "$REF" -a -dllib -lmystub ext.cmo -o ext.cma >/dev/null 2>&1
  both lib_stub_dll prog -- -I . ext.cma -o prog
  (cd "$OUT/lib_stub_dll/o" 2>/dev/null && CAML_LD_LIBRARY_PATH="$W/lib_stub_dll" timeout 20 "$RUN" ./prog > run 2>&1)
  (cd "$OUT/lib_stub_dll/c" 2>/dev/null && CAML_LD_LIBRARY_PATH="$W/lib_stub_dll" timeout 20 "$RUN" ./prog > run 2>&1)
  verdict lib_stub_dll; export OCAMLLIB="$ROOT/stdlib"
}

ALL="plain g g_nog linkall hints dllpath launch_exe launch_exe_bindir launch_sh launch_binsh search_fallback_exe
search_enable_exe use_runtime runtime_variant without_runtime stdlib_default unix_str unix_str_g threads noautolink
lib lib_g lib_linkall lib_cstuff lib_custom lib_of_lib link_lib link_lib_unused link_lib_dllib err_missing
err_notobj err_order err_dup err_noimpl err_inconsistent err_prim err_wrongname err_dll err_lib_missing custom
custom_stub camlprimc output_c output_obj output_complete_obj output_complete_exe output_complete_exe_stub
make_runtime custom_unix cfile_only cfile_o lib_stub_dll"
if [ $# -gt 0 ]; then cases="$*"; else cases="$ALL"; fi
for c in $cases; do "c_$c"; done
printf '%s\n' "${results[@]}" | tee "$OUT/results"
printf '%s\n' "${results[@]}" | awk '{f[$1]++} END{printf "cases %d: SAME %d  NONDET-OK %d  DIFF %d  SKIP %d\n", NR, f["SAME"], f["NONDET-OK"], f["DIFF"], f["SKIP"]}'
