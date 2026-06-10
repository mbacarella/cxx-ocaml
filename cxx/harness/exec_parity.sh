#!/usr/bin/env bash
# Execution parity: compile each corpus file with ocamlc.opt (oracle) and
# c++ocamlc, run both bytecode executables under runtime/ocamlrun, and compare
# stdout, stderr and exit status.  This measures the project's actual goal — a
# drop-in compiler producing programs that behave identically — rather than
# byte-exact IR dumps (a stricter bar that semantically-equivalent code fails).
#
# Each side compiles a copy of the file in its own temp dir under the file's
# basename, so embedded source paths (Assert_failure locations etc.) agree.
# Files the oracle cannot compile stand-alone (multi-module tests, otherlibs
# deps, intended type errors) or whose oracle binary times out are SKIPped.
#
# Usage: exec_parity.sh [N]      (JOBS=, CPP_TIMEOUT=, CACHE= overridable)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=$ROOT/cxx/build/c++ocamlc
RUN=$ROOT/runtime/ocamlrun
JOBS="${JOBS:-4}"
TIMEOUT="${CPP_TIMEOUT:-10}"
CACHE="${CACHE:-/tmp/exec_oracle_cache}"
MAXOUT=1000000   # cap captured output (bytes) per stream
mkdir -p "$CACHE"
key() { printf '%s' "$1" | tr '/' '%'; }

# Compile $1 with the compiler command in $2... and run it; print
#   <exit>\n<stdout bytes>\n--STDERR--\n<stderr bytes>   (or COMPILE_FAIL / RUN_TIMEOUT)
compile_run() {
  local f=$1; shift
  local td; td=$(mktemp -d) || return 1
  cp "$f" "$td/$(basename "$f")"
  ( cd "$td" && timeout "$TIMEOUT" "$@" "$(basename "$f")" -o o.exe ) >/dev/null 2>&1
  if [ ! -f "$td/o.exe" ]; then rm -rf "$td"; echo COMPILE_FAIL; return; fi
  ( cd "$td" && ulimit -f $((MAXOUT / 512)) 2>/dev/null
    timeout "$TIMEOUT" "$RUN" ./o.exe </dev/null >out.1 2>out.2 )
  local rc=$?
  if [ $rc -eq 124 ]; then rm -rf "$td"; echo RUN_TIMEOUT; return; fi
  echo "$rc"
  head -c $MAXOUT "$td/out.1" 2>/dev/null
  echo "--STDERR--"
  head -c $MAXOUT "$td/out.2" 2>/dev/null
  rm -rf "$td"
}

if [ "${1:-}" == "--worker" ]; then
  f="$2"
  ck="$CACHE/$(key "$f")"
  if [ ! -f "$ck" ]; then
    compile_run "$f" "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" > "$ck"
  fi
  o_head=$(head -1 "$ck")
  case "$o_head" in
    COMPILE_FAIL|RUN_TIMEOUT) printf 'SKIP\n'; exit 0 ;;
  esac
  m=$(compile_run "$f" "$CPP" -I "$ROOT/stdlib")
  case "$(printf '%s' "$m" | head -1)" in
    COMPILE_FAIL) printf 'CPPERR %s\n' "$f"; exit 0 ;;
    RUN_TIMEOUT)  printf 'TIMEOUT %s\n' "$f"; exit 0 ;;
  esac
  if [ "$(cat "$ck")" = "$m" ]; then printf 'M\n'; else printf 'DIFF %s\n' "$f"; fi
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find testsuite/tests -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | awk '
  $1=="M"{m++} $1=="DIFF"{d++} $1=="CPPERR"{e++} $1=="TIMEOUT"{t++} $1=="SKIP"{s++}
  END{
    judged=m+d+e+t
    printf "runnable files: %d   MATCH: %d   DIFF: %d   cpp-compile-fail: %d   cpp-timeout: %d   (skip %d)\n", judged, m, d, e, t, s
    if (judged) printf "exec parity: %.1f%%\n", 100*m/judged
  }'
printf '%s\n' "$res" | awk '$1=="DIFF"{print $2}' > /tmp/.exec_diff_files
printf '%s\n' "$res" | awk '$1=="CPPERR"{print $2}' > /tmp/.exec_cpperr_files
