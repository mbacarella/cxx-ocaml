#!/usr/bin/env bash
# Compile-speed and memory benchmark: c++ocamlc against ocamlc.opt on the
# same work.  Each unit is compiled by both compilers, interleaved, REPS
# times, one process at a time; the median wall time and the peak RSS of
# each are recorded.  Both compilers run in their own scratch directory with
# the same flags and the same prepared inputs (a unit's .mli is compiled
# first, untimed, by the compiler under test).
#
# Usage: bench.sh [corpus ...]      (default: startup small compiler stdlib)
#   startup   a trivial unit compiled STARTUP_N times (fixed per-process cost)
#   small     every SMALL_STEP-th stamp probe (small files)
#   compiler  the compiler's own .ml / .mli (utils parsing typing bytecomp
#             file_formats lambda driver) with its Makefile's flags, against
#             the built tree's .cmi
#   stdlib    the stdlib units with stdlib/Makefile's flags (Compflags)
# Environment:
#   REPS=3            runs per unit and compiler (median taken)
#   SMALL_STEP=16     sampling of the stamp probes
#   STARTUP_N=50      runs of the trivial unit
#   CPP=, REF=        the compilers (default: cxx/build-release/c++ocamlc,
#                     ./ocamlc.opt)
#   NATIVE=1          c++ocamlopt against ocamlopt.opt: .cmx outputs, the
#                     compiler corpus with the native back end's sources
#                     (middle_end, asmcomp, the opt drivers), and the
#                     "link" corpus (every SMALL_STEP-th probe compiled,
#                     then only its link timed)
#   OUT=/tmp/bench    per-unit TSVs, summary.tsv
#   PHASES=1          also sum c++ocamlc's CPPCAML_PROFILE phase timers
#   BASELINE=<file>   a previous summary.tsv: print the ratio change
#   MAXRATIO=<x>      exit 1 when a corpus's total time ratio (c++ / ref)
#                     exceeds x (e.g. MAXRATIO=1.0: "not slower")
# Timing is wall clock ($EPOCHREALTIME; without it, bash < 5, perl's
# Time::HiRes) around the process, including /usr/bin/time's fork (the same
# for both).  The peak RSS: GNU time's %M, else BSD time's -l (macOS).
# Don't run it beside a build or another harness: it measures, it doesn't
# isolate.
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
cd "$ROOT" || exit 1
NATIVE="${NATIVE:-}"
if [ -n "$NATIVE" ]; then
  CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlopt}"
  REF="${REF:-$ROOT/ocamlopt.opt}"
  OBJ=cmx
else
  CPP="${CPP:-$ROOT/cxx/build-release/c++ocamlc}"
  REF="${REF:-$ROOT/ocamlc.opt}"
  OBJ=cmo
fi
REPS="${REPS:-3}"
SMALL_STEP="${SMALL_STEP:-16}"
STARTUP_N="${STARTUP_N:-50}"
OUT="${OUT:-/tmp/bench}"
PHASES="${PHASES:-}"
BASELINE="${BASELINE:-}"
MAXRATIO="${MAXRATIO:-}"
TIME=/usr/bin/time
[ -x "$TIME" ] || { echo "bench.sh: needs $TIME for the peak RSS" >&2; exit 2; }
if "$TIME" -f %M -o /dev/null true 2>/dev/null; then GNU_TIME=1; else GNU_TIME=; fi
if [ $# -gt 0 ]; then corpora=("$@"); elif [ -n "${NATIVE:-}" ]; then corpora=(startup small compiler stdlib link); else corpora=(startup small compiler stdlib); fi
rm -rf "$OUT"; mkdir -p "$OUT"
W=$(mktemp -d)
trap 'rm -rf "${W:?}"' EXIT
vm_limit 16000000

# one timed run: prints "<ms> <rss_kb> <rc>"
run1() {
  local dir="$1"; shift
  if [ -z "${EPOCHREALTIME:-}" ]; then run1_perl "$dir" "$@"; return; fi
  local t0 t1 rc
  t0=$EPOCHREALTIME
  ( cd "$dir" && "$TIME" -f %M -o "$W/rss" "$@" ) >/dev/null 2>&1
  rc=$?
  t1=$EPOCHREALTIME
  awk -v a="$t0" -v b="$t1" -v r="$(tail -1 "$W/rss" 2>/dev/null || echo 0)" -v c="$rc" \
    'BEGIN { printf "%.3f %d %d\n", (b - a) * 1000, r, c }'
}
# the same, timed by perl; BSD time -l reports the peak RSS in bytes, on its
# stderr, which the command's own stderr must not reach (hence the sh -c
# exec: the measured process is the command's)
run1_perl() {
  local dir="$1"; shift
  local -a cmd
  if [ -n "$GNU_TIME" ]; then cmd=("$TIME" -f %M -o "$W/rss" "$@")
  else cmd=("$TIME" -l sh -c 'exec "$@" 2>/dev/null' sh "$@"); fi
  ( cd "$dir" && perl -MTime::HiRes=time -e '
      open(my $res, ">&", \*STDOUT) or die; open(STDOUT, ">", "/dev/null") or die;
      my $t0 = time; my $st = system { $ARGV[0] } @ARGV; my $t1 = time;
      printf $res "%.3f %d\n", ($t1 - $t0) * 1000, $st == -1 ? 127 : $st & 127 ? 128 + ($st & 127) : $st >> 8;
    ' "${cmd[@]}" 2>"$W/time.err" >"$W/run1" ) </dev/null
  local ms rc rss
  read -r ms rc < "$W/run1"
  if [ -n "$GNU_TIME" ]; then rss=$(tail -1 "$W/rss" 2>/dev/null || echo 0)
  else rss=$(awk '/maximum resident set size/ { printf "%d", $1 / 1024 }' "$W/time.err"); fi
  echo "$ms ${rss:-0} $rc"
}

median() { sort -n | awk '{ v[NR] = $1 } END { if (NR == 0) print 0; else print v[int((NR + 1) / 2)] }'; }

# bench_unit <corpus> <name> <ref_dir> <cpp_dir> <args...>: REPS interleaved
# runs of both compilers on the same arguments, one TSV row
bench_unit() {
  local corpus="$1" name="$2" rdir="$3" cdir="$4"; shift 4
  local rt=() ct=() rr=0 cr=0 rrc=0 crc=0 line ms rss rc i
  for ((i = 0; i < REPS; i++)); do
    read -r ms rss rc < <(run1 "$rdir" "$REF" -nostdlib -I "$ROOT/stdlib" "$@")
    rt+=("$ms"); [ "$rss" -gt "$rr" ] && rr=$rss; rrc=$rc
    read -r ms rss rc < <(run1 "$cdir" "$CPP" -I "$ROOT/stdlib" "$@")
    ct+=("$ms"); [ "$rss" -gt "$cr" ] && cr=$rss; crc=$rc
  done
  local rm cm
  rm=$(printf '%s\n' ${rt[@]+"${rt[@]}"} | median)
  cm=$(printf '%s\n' ${ct[@]+"${ct[@]}"} | median)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$rm" "$cm" "$rr" "$cr" "$rrc" "$crc" >> "$OUT/$corpus.tsv"
  if [ -n "$PHASES" ]; then
    ( cd "$cdir" && CPPCAML_PROFILE=1 "$CPP" -I "$ROOT/stdlib" "$@" ) 2>&1 >/dev/null |
      awk -v c="$corpus" '/^  [a-z][a-z ()]*: [0-9.]+ ms$/ {
                           l = $0; sub(/^  /, "", l); split(l, a, ": "); v = a[2]; sub(/ ms$/, "", v)
                           printf "%s\t%s\t%s\n", c, a[1], v }' >> "$OUT/phases.tsv"
  fi
}

# fresh <dir>: an empty scratch directory
fresh() { rm -rf "$1"; mkdir -p "$1"; }

corpus_startup() {
  local r="$W/su/r" c="$W/su/c"
  fresh "$r"; fresh "$c"
  echo 'let x = 1' > "$r/t.ml"; cp "$r/t.ml" "$c/"
  local i
  for ((i = 0; i < STARTUP_N; i++)); do
    REPS=1 bench_unit startup "t.ml#$i" "$r" "$c" -w -a -c t.ml
  done
}

corpus_small() {
  local f b r c
  local i=0
  for f in cxx/harness/stamp_probes/*.ml; do
    i=$((i + 1)); [ $((i % SMALL_STEP)) -eq 0 ] || continue
    b=$(basename "$f"); r="$W/sm/r"; c="$W/sm/c"
    fresh "$r"; fresh "$c"; cp "$f" "$r/"; cp "$f" "$c/"
    bench_unit small "$b" "$r" "$c" -w -a -c "$b"
  done
}

corpus_link() {
  local f b r c
  local i=0
  for f in cxx/harness/stamp_probes/*.ml; do
    i=$((i + 1)); [ $((i % SMALL_STEP)) -eq 0 ] || continue
    b=$(basename "$f"); r="$W/li/r"; c="$W/li/c"
    fresh "$r"; fresh "$c"; cp "$f" "$r/"; cp "$f" "$c/"
    # both link the same object, compiled once by the reference (untimed)
    ( cd "$r" && "$REF" -nostdlib -I "$ROOT/stdlib" -w -a -c "$b" ) >/dev/null 2>&1 || continue
    cp "$r/${b%.ml}".* "$c/"
    bench_unit link "$b" "$r" "$c" -o a.out "${b%.ml}.$OBJ"
  done
}

corpus_compiler() {
  local inc="" d f b r c dirs="utils parsing typing bytecomp file_formats lambda driver"
  [ -n "$NATIVE" ] && dirs="$dirs middle_end middle_end/closure asmcomp"
  for d in utils parsing typing bytecomp file_formats lambda middle_end middle_end/closure asmcomp driver toplevel; do inc="$inc -I $ROOT/$d"; done
  # the compiler's own flags (Makefile.build_config OC_COMMON_COMPFLAGS)
  # without -bin-annot and -warn-error
  local fl="-g -strict-sequence -principal -absname -w +a-4-9-40-41-42-44-45-48 -alert @ocaml_deprecated_cli -strict-formats"
  for f in $(for d in $dirs; do ls $d/*.ml; done); do
    [ -z "$NATIVE" ] && case "$f" in *_native*|*/optmain*|*/optcompile*|*/optmaindriver*|*/opterrors*) continue ;; esac
    b=$(basename "$f"); r="$W/co/r"; c="$W/co/c"
    # a unit ocamlc can't compile alone here (missing native-only deps) is skipped
    fresh "$r"; cp "$f" "$r/"
    [ -f "${f}i" ] && cp "${f}i" "$r/"
    if [ -f "${f}i" ]; then
      ( cd "$r" && "$REF" -nostdlib -I "$ROOT/stdlib" $inc $fl -c "${b}i" ) >/dev/null 2>&1 || continue
    fi
    ( cd "$r" && "$REF" -nostdlib -I "$ROOT/stdlib" $inc $fl -c "$b" ) >/dev/null 2>&1 || continue
    fresh "$r"; fresh "$c"
    cp "$f" "$r/"; cp "$f" "$c/"
    if [ -f "${f}i" ]; then
      cp "${f}i" "$r/"; cp "${f}i" "$c/"
      bench_unit compiler "${b}i" "$r" "$c" $inc $fl -c "${b}i"
    fi
    bench_unit compiler "$b" "$r" "$c" $inc $fl -c "$b"
  done
}

corpus_stdlib() {
  local comp="-strict-sequence -absname -w +a-4-9-41-42-44-45-48 -g -nostdlib -principal"
  local m base tgt cflags iflags r c
  for m in stdlib/*.ml; do
    m=$(basename "$m"); base=${m%.ml}
    case "$base" in
      camlinternal*|std_exit|stdlib) tgt=$base ;;
      *) tgt=stdlib__$(echo "${base:0:1}" | tr a-z A-Z)${base:1} ;;
    esac
    cflags=$(cd stdlib && AWK=awk sh ./Compflags "$tgt.cmo" | sed "s|\./expand_module_aliases.awk|$ROOT/stdlib/expand_module_aliases.awk|")
    iflags=$(cd stdlib && AWK=awk sh ./Compflags "$tgt.cmi" | sed "s|\./expand_module_aliases.awk|$ROOT/stdlib/expand_module_aliases.awk|")
    r="$W/st/r"; c="$W/st/c"
    fresh "$r"; fresh "$c"
    cp "stdlib/$m" "$r/"; cp "stdlib/$m" "$c/"
    if [ -f "stdlib/$base.mli" ]; then
      cp "stdlib/$base.mli" "$r/"; cp "stdlib/$base.mli" "$c/"
      eval "bench_unit stdlib $base.mli $r $c $comp $iflags -o $tgt.cmi -c $base.mli"
    fi
    eval "bench_unit stdlib $m $r $c $comp $cflags -o $tgt.$OBJ -c $m"
  done
}

export AWK=awk
for c in ${corpora[@]+"${corpora[@]}"}; do
  case "$c" in
    startup|small|compiler|stdlib|link) ;;
    *) echo "bench.sh: unknown corpus $c" >&2; exit 2 ;;
  esac
  printf 'unit\tref_ms\tcpp_ms\tref_rss_kb\tcpp_rss_kb\tref_rc\tcpp_rc\n' > "$OUT/$c.tsv"
  echo "== $c" >&2
  "corpus_$c"
done

# summary: units both compilers compiled; totals, ratio of totals, geometric
# mean of per-unit ratios, the peak RSS of each, the five slowest units
# relative to ocamlc.opt (among units taking >= 5 ms there)
printf 'corpus\tunits\tref_ms\tcpp_ms\tratio\tgeomean\tref_rss_max_kb\tcpp_rss_max_kb\n' > "$OUT/summary.tsv"
status=0
for c in ${corpora[@]+"${corpora[@]}"}; do
  awk -F'\t' -v c="$c" 'NR > 1 && $6 == 0 && $7 == 0 {
      n++; r += $2; p += $3; if ($2 > 0 && $3 > 0) { lg += log($3 / $2); m++ }
      if ($4 > rr) rr = $4; if ($5 > cr) cr = $5 }
    END { printf "%s\t%d\t%.1f\t%.1f\t%.3f\t%.3f\t%d\t%d\n", c, n, r, p, (r > 0 ? p / r : 0),
                 (m > 0 ? exp(lg / m) : 0), rr, cr }' "$OUT/$c.tsv" >> "$OUT/summary.tsv"
done
echo
column -t -s $'\t' "$OUT/summary.tsv"
for c in ${corpora[@]+"${corpora[@]}"}; do
  bad=$(awk -F'\t' 'NR > 1 && $6 != $7' "$OUT/$c.tsv" | wc -l)
  [ "$bad" -gt 0 ] && echo "$c: $bad unit(s) where the compilers' exit codes differ (not counted)"
  echo "$c: slowest relative to $(basename "$REF"):"
  awk -F'\t' 'NR > 1 && $6 == 0 && $7 == 0 && $2 >= 5 { printf "  %6.2fx  %8.1f ms  %8.1f ms  %s\n", $3 / $2, $2, $3, $1 }' \
    "$OUT/$c.tsv" | sort -rn | head -5
done
if [ -n "$PHASES" ] && [ -s "$OUT/phases.tsv" ]; then
  echo; echo "c++ocamlc phases (CPPCAML_PROFILE, ms, summed per corpus):"
  awk -F'\t' '{ s[$1 "\t" $2] += $3 } END { for (k in s) printf "%s\t%.1f\n", k, s[k] }' "$OUT/phases.tsv" |
    sort | column -t -s $'\t' | sed 's/^/  /'
fi
if [ -n "$BASELINE" ] && [ -f "$BASELINE" ]; then
  echo; echo "ratio vs baseline $BASELINE:"
  awk -F'\t' 'FNR == 1 { next } NR == FNR { b[$1] = $5; next }
    ($1 in b) { printf "  %-9s %.3f -> %.3f\n", $1, b[$1], $5 }' "$BASELINE" "$OUT/summary.tsv"
fi
if [ -n "$MAXRATIO" ]; then
  while IFS=$'\t' read -r c _ _ _ ratio _; do
    [ "$c" = corpus ] && continue
    if awk -v r="$ratio" -v m="$MAXRATIO" 'BEGIN { exit !(r > m) }'; then
      echo "FAIL: $c: c++ocamlc / ocamlc.opt = $ratio > $MAXRATIO"; status=1
    fi
  done < "$OUT/summary.tsv"
fi
echo "per-unit TSVs and summary.tsv in $OUT"
exit $status
