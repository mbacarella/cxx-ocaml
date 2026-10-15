#!/usr/bin/env bash
# Line, branch and function coverage of c++ocamlc / c++ocamlopt (gcc's
# --coverage and gcov): which parts of the port -- and so of the upstream
# compiler functions it ports one for one -- a workload executes.  Made to
# find what a random program generator (camlsmith port mode) never reaches,
# against a baseline such as the testsuite or the stdlib.  Linux and gcc
# (gcov -j, gcc >= 9) only.
#
# Usage:
#   coverage.sh build                 the instrumented compilers, in cxx/$O
#                                     (default build-cov: -O1 -g --coverage;
#                                     the tree's configuration, as cxx/Makefile;
#                                     a flambda tree gives a flambda c++ocamlopt)
#   coverage.sh run SET CMD...        CMD with the counters written to the set
#                                     SET (GCOV_PREFIX), the compilers' fast
#                                     exit off (it skips gcov's dump)
#   coverage.sh collect SET...        gcov over each set: $COVDIR/prof/SET.json
#                                     (per source line, branch and function)
#   coverage.sh table A[,A2..] [B[,B2..]] [FILE...]
#                                     per file: lines, % reached by A, by B,
#                                     by either; branch %; functions reached
#   coverage.sh detail A B FILE...    per FILE: the functions A never reaches
#                                     (with B's call count), the unreached
#                                     blocks inside reached functions ([B]: B
#                                     reaches them), and the lines whose
#                                     branches A takes only partly
#   FILE: as cxx/ names it, e.g. src/typing/cmm_helpers.cpp.
#   COVDIR= the sets and profiles (default /tmp/cxx-coverage); O= the build
#   directory under cxx/ (several sets of one build: one run each).
#
# Example (camlsmith against the testsuite):
#   coverage.sh build
#   coverage.sh run cs camlsmith port --root $ROOT \
#     --candidate-ocamlc cxx/build-cov/c++ocamlc \
#     --candidate-ocamlopt cxx/build-cov/c++ocamlopt --levels nat -n 1000
#   coverage.sh run ts sh -c 'for f in testsuite/tests/*/*.ml; do ...; done'
#   coverage.sh collect cs ts && coverage.sh table cs ts
set -u
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
. "$(dirname "$SELF")/portable.sh"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd -P)"
O="${O:-build-cov}"
COVDIR="${COVDIR:-/tmp/cxx-coverage}"
BUILD="$ROOT/cxx/$O"
PY="$COVDIR/coverage.py"

write_py() {
  mkdir -p "$COVDIR"
  cat > "$PY" <<'EOF'
import gzip, json, os, re, sys

def norm(p):
    i = p.rfind('cxx/')
    if i >= 0: p = p[i + 4:]
    return p if p.startswith(('src/', 'include/')) else None

def addbr(old, new):
    if old is None: return list(new)
    if len(old) < len(new): old, new = new, old
    return [a + (new[i] if i < len(new) else 0) for i, a in enumerate(old)]

def merge(tot, prof):
    for f, P in prof.items():
        T = tot.setdefault(f, {"l": {}, "b": {}, "f": {}})
        for k, v in P["l"].items(): T["l"][k] = T["l"].get(k, 0) + v
        for k, v in P["b"].items(): T["b"][k] = addbr(T["b"].get(k), v)
        for k, v in P["f"].items():
            e = T["f"].setdefault(k, [0, v[1], v[2]]); e[0] += v[0]

def collect(out, d):
    prof = {}
    for root, _, files in os.walk(d):
        for fn in files:
            if not fn.endswith('.gcov.json.gz'): continue
            with gzip.open(os.path.join(root, fn)) as fh: j = json.load(fh)
            one = {}
            for fe in j["files"]:
                f = norm(fe["file"])
                if f is None: continue
                P = one.setdefault(f, {"l": {}, "b": {}, "f": {}})
                for fu in fe["functions"]:
                    k = "%d:%s" % (fu["start_line"], fu.get("demangled_name", fu["name"]))
                    e = P["f"].setdefault(k, [0, fu["start_line"], fu["end_line"]]); e[0] += fu["execution_count"]
                for ln in fe["lines"]:
                    n = str(ln["line_number"])
                    P["l"][n] = P["l"].get(n, 0) + ln["count"]
                    bs = [b["count"] for b in ln.get("branches", []) if not b.get("throw")]
                    if bs: P["b"][n] = addbr(P["b"].get(n), bs)
            merge(prof, one)
    json.dump(prof, open(out, "w"))

def load(arg, pdir):
    tot = {}
    for s in arg.split(','):
        merge(tot, json.load(open(os.path.join(pdir, s + ".json"))))
    return tot

def pct(a, b): return "%5.1f" % (100.0 * a / b) if b else "    -"
E = {"l": {}, "b": {}, "f": {}}

def table(A, B, files):
    print("%-46s %6s %6s %6s %6s | %6s %6s | %9s" % ("file", "lines", "A%", "B%", "A|B%", "brA%", "brB%", "fn A/all"))
    T = [0, 0, 0, 0]
    for f in (files or sorted(set(A) | set(B))):
        a, b = A.get(f, E), B.get(f, E)
        L = set(a["l"]) | set(b["l"])
        if not L: continue
        ha = sum(1 for k in L if a["l"].get(k)); hb = sum(1 for k in L if b["l"].get(k))
        hu = sum(1 for k in L if a["l"].get(k) or b["l"].get(k))
        bra = [x for v in a["b"].values() for x in v]; brb = [x for v in b["b"].values() for x in v]
        T[0] += len(L); T[1] += ha; T[2] += hb; T[3] += hu
        print("%-46s %6d %6s %6s %6s | %6s %6s | %9s" % (f[-46:], len(L), pct(ha, len(L)), pct(hb, len(L)), pct(hu, len(L)),
              pct(sum(1 for x in bra if x), len(bra)), pct(sum(1 for x in brb if x), len(brb)),
              "%d/%d" % (sum(1 for v in a["f"].values() if v[0]), len(a["f"]))))
    print("%-46s %6d %6s %6s %6s" % ("TOTAL", T[0], pct(T[1], T[0]), pct(T[2], T[0]), pct(T[3], T[0])))

def detail(A, B, files):
    for f in files:
        a, b = A.get(f, E), B.get(f, E)
        text = open(os.path.join(sys.argv[2], f), errors="replace").read().split("\n")
        def short(k): return re.sub(r"cppcaml::typing::", "", k.split(":", 1)[1])[:110]
        print("=" * 80 + "\n" + f + "\n-- functions A never reaches (B: B's calls)")
        fns = sorted(set(a["f"]) | set(b["f"]), key=lambda k: int(k.split(":", 1)[0]))
        for k in fns:
            if a["f"].get(k, [0])[0] == 0:
                s, e = (a["f"].get(k) or b["f"][k])[1:]
                print("  %5d-%-5d B=%-8d %s" % (s, e, b["f"].get(k, [0])[0], short(k)))
        print("-- unreached blocks of reached functions ([B]: B reaches them)")
        for k in fns:
            c = a["f"].get(k, [0])[0]
            if c == 0: continue
            s, e = a["f"][k][1:]
            blk = [n for n in range(s, e + 1) if a["l"].get(str(n), 1) == 0]
            if not blk: continue
            print("  %s  (calls %d)" % (short(k), c))
            groups, cur = [], [blk[0]]
            for n in blk[1:]:
                if any(str(m) in a["l"] for m in range(cur[-1] + 1, n)): groups.append(cur); cur = [n]
                else: cur.append(n)
            groups.append(cur)
            for g in groups:
                bh = any(b["l"].get(str(n), 0) for n in g)
                print("    %5d-%-5d %s %s" % (g[0], g[-1], "[B]" if bh else "   ", text[g[0] - 1].strip()[:90]))
        print("-- partly taken branches (counts per branch, A then B)")
        for n in sorted(a["b"], key=int):
            ca, cb = a["b"][n], b["b"].get(n, [])
            if a["l"].get(n, 0) and any(x == 0 for x in ca):
                print("  %5s A=%s B=%s  %s" % (n, ca[:10], cb[:10], text[int(n) - 1].strip()[:100]))

cmd = sys.argv[1]
if cmd == "collect": collect(sys.argv[3], sys.argv[4])
else:
    pdir = sys.argv[3]
    A = load(sys.argv[4], pdir)
    B = load(sys.argv[5], pdir) if len(sys.argv) > 5 and sys.argv[5] != "-" else {}
    (table if cmd == "table" else detail)(A, B, sys.argv[6:])
EOF
}

case "${1:-}" in
  build)
    vm_limit 32000000
    make -C "$ROOT/cxx" -j "${JOBS:-4}" O="$O" OPTFLAGS="-O1 -g -DNDEBUG --coverage" LDFLAGS=--coverage \
      c++ocamlc c++ocamlopt ;;
  run)
    [ $# -ge 3 ] || { echo "usage: coverage.sh run SET CMD..." >&2; exit 2; }
    set_="$2"; shift 2
    mkdir -p "$COVDIR/gcda/$set_"
    GCOV_PREFIX="$COVDIR/gcda/$set_" GCOV_PREFIX_STRIP=0 CPPCAML_NO_FASTEXIT=1 "$@" ;;
  collect)
    shift; write_py
    for set_ in "$@"; do
      g="$COVDIR/gcda/$set_$BUILD"
      [ -d "$g" ] || { echo "$set_: no counters of $BUILD" >&2; continue; }
      t="$COVDIR/json/$set_"; rm -rf "$t"; mkdir -p "$t" "$COVDIR/prof"
      # gcov wants each .gcno beside its .gcda; one directory per unit, as
      # gcov names its output after the source
      find "$g" -name '*.gcda' | while IFS= read -r d; do
        n="${d%.gcda}.gcno"; cp -f "$BUILD/${n#$g/}" "$n"
        u="$t/$(echo "$d" | cksum | cut -d' ' -f1)"; mkdir -p "$u"
        ( cd "$u" && gcov -j -b "$d" >/dev/null 2>&1 ) || echo "gcov failed: $d" >&2
      done
      python3 "$PY" collect "$ROOT/cxx" "$COVDIR/prof/$set_.json" "$t" && rm -rf "$t"
      echo "$set_: $COVDIR/prof/$set_.json"
    done ;;
  table|detail)
    [ $# -ge 2 ] || { echo "usage: coverage.sh $1 A[,A2..] [B[,B2..]] [FILE...]" >&2; exit 2; }
    write_py
    python3 "$PY" "$1" "$ROOT/cxx" "$COVDIR/prof" "$2" "${3:--}" "${@:4}" ;;
  *) sed -n '2,/^set -u/p' "$SELF" | sed '$d'; exit 2 ;;
esac
