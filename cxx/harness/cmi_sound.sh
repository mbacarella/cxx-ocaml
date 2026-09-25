#!/usr/bin/env bash
# SOUNDNESS of our .cmi against ocamlc.opt's, over the testsuite corpus.
#
# Each file is compiled twice under different unit names -- ours_<f> by
# c++ocamlc, ref_<f> by ocamlc.opt -- and ocamlc.opt then checks the two
# signatures against each other in both directions:
#
#   module C : module type of Ours_f = Ref_f   fails => LOOSER: our .cmi
#       promises more than the real one (a consumer ocamlc would reject
#       compiles against ours -- a soundness hole)
#   module C : module type of Ref_f = Ours_f   fails => STRICTER: a valid
#       consumer is rejected against ours
#
# A file whose .cmi fails the same check against a second ocamlc copy of
# itself counts as NOISE: `module type of` cannot restate its signature.
#
#   cmi_sound.sh [N]        JOBS=, CPP=, KEEP=dir (keep each failure's
#                           checker message in dir/<class>/<path>.txt),
#                           DIR= (default testsuite/tests; the stamp probes
#                           cxx/harness/stamp_probes are a far wider net)
set -u
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/../.." || exit 1
ROOT=$PWD
CPP=${CPP:-$ROOT/cxx/build-release/c++ocamlc}
JOBS="${JOBS:-8}"
TIMEOUT="${CPP_TIMEOUT:-20}"

if [ "${1:-}" = "--worker" ]; then
  f="$2"
  b=$(basename "$f" .ml)
  case "$b" in *[!a-z0-9_]*) echo "SKIP $f"; exit 0 ;; esac
  td=$(mktemp -d) || exit 0
  cp "$f" "$td/ref_$b.ml"; cp "$f" "$td/ours_$b.ml"; cp "$f" "$td/ref2_$b.ml"
  cd "$td" || exit 0
  for u in ref ref2; do
    timeout $TIMEOUT "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -w -a \
      -c "${u}_$b.ml" >/dev/null 2>&1
  done
  if [ ! -f "ref_$b.cmi" ]; then cd /; rm -rf "$td"; echo "SKIP $f"; exit 0; fi
  timeout $TIMEOUT "$CPP" -nostdlib -I "$ROOT/stdlib" -w -a \
    -c "ours_$b.ml" >/dev/null 2>&1
  if [ ! -f "ours_$b.cmi" ]; then
    cd /; rm -rf "$td"; echo "CPPERR $f"; exit 0
  fi
  O="Ours_$b"; R="Ref_$b"
  # The control: ocamlc's own .cmi against a second copy of itself.  Where
  # even that fails, `module type of` cannot restate the signature and the
  # file says nothing about ours.
  echo "module C : module type of Ref2_$b = $R" > chk_ctl.ml
  if ! timeout $TIMEOUT "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -I . \
       -w -a -c chk_ctl.ml >/dev/null 2>&1; then
    cd /; rm -rf "$td"; echo "NOISE $f"; exit 0
  fi
  echo "module C : module type of $O = $R" > chk_loose.ml
  echo "module C : module type of $R = $O" > chk_strict.ml
  loose=0; strict=0
  timeout $TIMEOUT "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -I . \
    -w -a -c chk_loose.ml > loose.txt 2>&1 || loose=1
  timeout $TIMEOUT "$ROOT/ocamlc.opt" -nostdlib -I "$ROOT/stdlib" -I . \
    -w -a -c chk_strict.ml > strict.txt 2>&1 || strict=1
  cls=EQUIV
  [ $loose = 1 ] && cls=LOOSER
  [ $strict = 1 ] && cls=STRICTER
  [ $loose = 1 ] && [ $strict = 1 ] && cls=BOTH
  if [ -n "${KEEP:-}" ] && [ $cls != EQUIV ]; then
    k="$KEEP/$cls/${f//\//__}.txt"; mkdir -p "$KEEP/$cls"
    { echo "== LOOSER check"; cat loose.txt
      echo "== STRICTER check"; cat strict.txt; } > "$k"
  fi
  cd /; rm -rf "$td"
  echo "$cls $f"
  exit 0
fi

LIMIT="${1:-0}"
mapfile -t files < <(find "${DIR:-testsuite/tests}" -name '*.ml' | sort)
[ "$LIMIT" -gt 0 ] 2>/dev/null && files=("${files[@]:0:$LIMIT}")
res=$(printf '%s\n' "${files[@]}" \
      | xargs -P "$JOBS" -I{} bash "$SELF" --worker {})
printf '%s\n' "$res" | sort > /tmp/.cmi_sound_results
awk '{n[$1]++} $1!="SKIP"&&$1!="CPPERR"{j++}
  END{printf "cmi judged: %d   EQUIV: %d   LOOSER: %d   STRICTER: %d" \
             "   BOTH: %d   (noise %d, cpp-fail %d, skip %d)\n",
             j-n["NOISE"], n["EQUIV"], n["LOOSER"], n["STRICTER"], n["BOTH"],
             n["NOISE"],
             n["CPPERR"], n["SKIP"]}' /tmp/.cmi_sound_results
