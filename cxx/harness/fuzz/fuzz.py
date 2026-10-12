#!/usr/bin/env python3
"""Differential fuzzing of the port against stock OCaml, at every level.

    fuzz.py [--start S] [--count N] [--jobs J] [--ill] [--mf] [--levels L,...]
            [--out DIR]

--mf: multi-file programs (genmf.py: units with interfaces, linked,
packed and archived), levels mbyte/mnat/mpack/mlib/mfl/mwp (compare_mf);
with --ill, one unchecked interface edit (signature-matching errors).

For each seed, gen.py writes a random well-typed program (--ill: one
ill-typed expression).  The program is compiled by the tree's stock
compilers (ocamlc.opt, ocamlopt.opt) and by the port (c++ocamlc,
c++ocamlopt), each in the same directory one after the other so embedded
paths agree, and every output the port claims identical is compared:

  i         -i (stdout, stderr, exit)
  bc        -c -bin-annot -dparsetree -drawlambda -dlambda -dinstr (stdout,
            stderr, exit); then the .cmi, .cmo and .cmt bytes
  bcg       -g -c: the .cmo bytes
  byte      link: executable bytes; run under runtime/ocamlrun: output
  nat       ocamlopt -c -S -drawclambda -dclambda -dcmm -dlinear (stdout,
            stderr, exit); the .s, .cmx and .o bytes; link and run: output
  fl        the flambda tree (FLAMBDA_ROOT, default ~/code/c++caml-mono-fuzz):
            stock ocamlopt.opt vs c++ocamlopt at the seed's flambda flags
            (-O3 by default), -dflambda + .s + run
  wp        whole-program mode there (CPPCAML_WP=1 c++ocamlopt -O3): run
            output against stock bytecode's

Flags (flags_for): common ones by seed % 11 (-unsafe, -unboxed-types,
-principal, -g, ...) and each back end's own by seed // 11, written as
`nat:F` (closure ocamlopt only: -linscan, -inline, -compact, ...) and
`fl:F` (flambda: -O2/-O3/-Oclassic, -unbox-closures, ...) tokens, so a
finding's info line and a regress header carry them as they are.

A stock bytecode / stock native / stock flambda run disagreement is a
generator problem (unspecified behaviour), reported as UNSPEC, not as a
port finding.  Findings go to DIR/findings/SEED-LEVEL/ with the program,
the marked program (minimize.py) and both sides' outputs.

Environment: ROOT (the closure tree, default this checkout), CPPC/CPPOPT
(default cxx/build-release/c++ocamlc, c++ocamlopt), MEM_KB (ulimit -v per
process, default 4000000), RUN_TIMEOUT (10), COMPILE_TIMEOUT (120).
"""
import argparse
import concurrent.futures as cf
import os
import resource
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('ROOT', os.path.abspath(os.path.join(HERE, '..', '..', '..')))
FL = os.environ.get('FLAMBDA_ROOT', os.path.expanduser('~/code/c++caml-mono-fuzz'))
CPPC = os.environ.get('CPPC', os.path.join(ROOT, 'cxx/build-release/c++ocamlc'))
CPPOPT = os.environ.get('CPPOPT', os.path.join(ROOT, 'cxx/build-release/c++ocamlopt'))
MEM_KB = int(os.environ.get('MEM_KB', '4000000'))
RUN_TIMEOUT = int(os.environ.get('RUN_TIMEOUT', '10'))
COMPILE_TIMEOUT = int(os.environ.get('COMPILE_TIMEOUT', '120'))
ALL_LEVELS = ['i', 'bc', 'bcg', 'byte', 'nat', 'fl', 'wp']
MAXOUT = 2_000_000

sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # (no __pycache__ in the tree)
import gen  # noqa: E402


def limits():
    resource.setrlimit(resource.RLIMIT_AS, (MEM_KB * 1024, MEM_KB * 1024))
    resource.setrlimit(resource.RLIMIT_STACK,
                       (resource.RLIM_INFINITY, resource.RLIM_INFINITY))


def run(cmd, cwd, timeout, env=None):
    """-> bytes 'rc\\n--OUT--\\nstdout--ERR--\\nstderr' (or TIMEOUT)."""
    e = dict(os.environ)
    e.pop('OCAMLRUNPARAM', None)
    e.pop('OCAMLPARAM', None)
    e.pop('CPPCAML_WP', None)
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout,
                           preexec_fn=limits, env=e)
    except subprocess.TimeoutExpired:
        return b'TIMEOUT'
    return b'%d\n--OUT--\n%s--ERR--\n%s' % (p.returncode, p.stdout[:MAXOUT],
                                          p.stderr[:MAXOUT])


def rc_of(res):
    if res == b'TIMEOUT':
        return None
    return int(res.split(b'\n', 1)[0])


def readb(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError:
        return b'<missing>'


def rm(paths):
    for p in paths:
        try:
            os.unlink(p)
        except OSError:
            pass


class Side:
    def __init__(self, name, byte, opt, flags):
        self.name = name
        self.byte = byte
        self.opt = opt
        self.flags = flags


def sides():
    std = ['-nostdlib', '-I', os.path.join(ROOT, 'stdlib')]
    return [Side('stock', os.path.join(ROOT, 'ocamlc.opt'),
                 os.path.join(ROOT, 'ocamlopt.opt'), std),
            Side('port', CPPC, CPPOPT, std)]


def fl_sides():
    std = ['-nostdlib', '-I', os.path.join(FL, 'stdlib')]
    return [Side('stock', None, os.path.join(FL, 'ocamlopt.opt'), std),
            Side('port', None, os.path.join(FL, 'cxx/build-release/c++ocamlopt'), std)]


def compare_levels(wd, levels, wflags=(), stop_at_first=False):
    """Compile fz.ml in wd on both sides at `levels`; return
    (findings, notes, ref_run) where findings = [(level, stock, port)]."""
    findings = []
    notes = []
    out = {}
    W, NW, FW = split_flags(wflags)

    bindir = os.path.join(wd, '.bin')
    os.makedirs(bindir, exist_ok=True)

    def link_side(s):
        # cmt_args records argv[0]: both sides run through the same path
        for name, target in (('ocamlc', s.byte), ('ocamlopt', s.opt)):
            lnk = os.path.join(bindir, name)
            rm([lnk])
            if target:
                os.symlink(target, lnk)
        return Side(s.name, os.path.join(bindir, 'ocamlc'),
                    os.path.join(bindir, 'ocamlopt'), s.flags)

    def both(level, mk, artifacts=(), tmo=COMPILE_TIMEOUT, env=None, run_exe=None):
        res = {}
        for s in sides_for(level):
            s = link_side(s)
            rm([os.path.join(wd, a) for a in artifacts])
            r = run(mk(s), wd, tmo, env)
            arts = {a: readb(os.path.join(wd, a)) for a in artifacts}
            ran = None
            if run_exe and rc_of(r) == 0:
                ran = run(run_exe(s), wd, RUN_TIMEOUT)
            res[s.name] = (r, arts, ran)
        return res

    def sides_for(level):
        return fl_sides() if level in ('fl',) else sides()

    def check(level, res, artifacts=()):
        st, pt = res['stock'], res['port']
        if st[0] != pt[0]:
            findings.append((level, st[0], pt[0]))
        for a in artifacts:
            if st[1][a] != pt[1][a]:
                if a.endswith('.cmt'):
                    ds, dp = cmtdump(wd, st[1][a]), cmtdump(wd, pt[1][a])
                    if ds == dp:
                        notes.append('cmt-sharing')
                        continue
                    findings.append(('%s:%s' % (level, a), ds, dp))
                    continue
                findings.append(('%s:%s' % (level, a), st[1][a], pt[1][a]))
        if st[2] is not None or pt[2] is not None:
            if st[2] != pt[2]:
                findings.append(('%s:run' % level, st[2] or b'', pt[2] or b''))

    stock_ok = None
    if 'i' not in levels and 'bc' not in levels:
        # whatever the levels, a program stock rejects stops here
        r = run([os.path.join(ROOT, 'ocamlc.opt'), '-nostdlib', '-I',
                 os.path.join(ROOT, 'stdlib')] + W + ['-i', 'fz.ml'], wd, COMPILE_TIMEOUT)
        if rc_of(r) != 0:
            notes.append('stock-rejects')
            return findings, notes, out
    if 'i' in levels:
        res = both('i', lambda s: [s.byte] + s.flags + W + ['-i', 'fz.ml'])
        check('i', res)
        stock_ok = rc_of(res['stock'][0]) == 0
    if stop_at_first and findings:
        return findings, notes, out
    if 'bc' in levels:
        arts = ('fz.cmi', 'fz.cmo', 'fz.cmt')
        res = both('bc', lambda s: [s.byte] + s.flags + W + [
            '-c', '-bin-annot', '-dparsetree', '-drawlambda', '-dlambda',
            '-dinstr', 'fz.ml'], arts)
        check('bc', res, arts)
        stock_ok = rc_of(res['stock'][0]) == 0
    if stop_at_first and findings:
        return findings, notes, out
    if stock_ok is False:
        notes.append('stock-rejects')
        return findings, notes, out
    if 'bcg' in levels:
        res = both('bcg', lambda s: [s.byte] + s.flags + W + ['-g', '-c', 'fz.ml'],
                   ('fz.cmo',))
        check('bcg', res, ('fz.cmo',))
    if stop_at_first and findings:
        return findings, notes, out
    if 'byte' in levels:
        res = both('byte', lambda s: [s.byte] + s.flags + W + ['fz.ml', '-o', 'fz.byte'],
                   ('fz.byte',), run_exe=lambda s: [os.path.join(ROOT, 'runtime/ocamlrun'),
                                                    './fz.byte'])
        check('byte', res, ('fz.byte',))
        out['byte'] = res['stock'][2]
    if stop_at_first and findings:
        return findings, notes, out
    if 'nat' in levels:
        arts = ('fz.s', 'fz.cmx', 'fz.o')
        res = both('nat', lambda s: [s.opt] + s.flags + W + [
            '-c', '-S', '-drawclambda', '-dclambda', '-dcmm', '-dlinear', 'fz.ml'] + NW, arts)
        check('nat', res, arts)
        res = both('natexe', lambda s: [s.opt] + s.flags + W + NW + ['fz.ml', '-o', 'fz.nat'],
                   ('fz.nat',), run_exe=lambda s: ['./fz.nat'])
        check('natexe', res, ('fz.nat',))
        out['nat'] = res['stock'][2]
    if stop_at_first and findings:
        return findings, notes, out
    if 'fl' in levels and os.path.isdir(FL):
        arts = ('fz.s', 'fz.cmx', 'fz.o')
        res = both('fl', lambda s: [s.opt] + s.flags + W + FW + [
            '-c', '-S', '-dflambda', 'fz.ml'], arts)
        check('fl', res, arts)
        res = both('fl', lambda s: [s.opt] + s.flags + W + FW + ['fz.ml', '-o', 'fz.fl'],
                   ('fz.fl',), run_exe=lambda s: ['./fz.fl'])
        check('flexe', res, ('fz.fl',))
        out['fl'] = res['stock'][2]
    if 'wp' in levels and os.path.isdir(FL):
        rm([os.path.join(wd, 'fz.wp')])
        r = run([os.path.join(FL, 'cxx/build-release/c++ocamlopt'), '-nostdlib', '-I',
                 os.path.join(FL, 'stdlib'), '-I', os.path.join(FL, 'runtime')] + wp_flags(W) +
                ['-O3', 'fz.ml', '-o', 'fz.wp'], wd, COMPILE_TIMEOUT * 3,
                env={'CPPCAML_WP': '1', 'TMPDIR': wd})
        if rc_of(r) != 0:
            findings.append(('wp:build', b'', r))
        else:
            out['wp'] = run(['./fz.wp'], wd, RUN_TIMEOUT)
    # cross-back-end: the reference is stock bytecode
    ref = out.get('byte')
    for k in ('nat', 'fl'):
        if ref is not None and out.get(k) is not None and out[k] != ref:
            notes.append('UNSPEC-%s' % k)
    if ref is not None and out.get('wp') is not None and out['wp'] != ref:
        unspec = any(n.startswith('UNSPEC') for n in notes)
        if ref == b'TIMEOUT' or out['wp'] == b'TIMEOUT':
            notes.append('wp-timeout')
        elif not unspec:
            findings.append(('wp:run', ref, out['wp']))
    return findings, notes, out


MF_LEVELS = ['mbyte', 'mnat', 'mpack', 'mlib', 'mfl', 'mwp']


def compare_mf(wd, levels, wflags=()):
    """The multi-file program in wd (genmf.py: DIR/order) at `levels`:
      mbyte  per unit, in order: -i, then -c -bin-annot of the .mli and the
             .ml; every .cmi/.cmo/.cmt/.cmti; link, executable bytes, run
      mnat   per unit -c -S: .cmx/.o/.s; link, executable, run
      mfl    the same with the flambda tree (FLAMBDA_ROOT) at -O3
      mpack  -for-pack Pk units, -pack into pk.cmo / pk.cmx (+ .cmi, .o),
             link the pack alone, run (bytecode and native)
      mlib   -a into lib.cma / lib.cmxa (+ .a), link with -linkall, run
      mwp    whole-program mode (CPPCAML_WP=1) over all units: run output
             against stock bytecode's
    -> (findings, notes, outputs) as compare_levels."""
    findings, notes, out = [], [], {}
    W = split_flags(wflags)[0]
    order = []
    mlionly = []
    with open(os.path.join(wd, 'order')) as f:
        for line in f:
            parts = line.split()
            if parts[1:] == ['mlionly']:
                mlionly.append(parts[0])
            else:
                order.append((parts[0], len(parts) > 1))
    sources = set(os.listdir(wd))
    bindir = os.path.join(wd, '.bin')
    os.makedirs(bindir, exist_ok=True)

    def clean():
        for f in os.listdir(wd):
            if f not in sources and f != '.bin':
                rm([os.path.join(wd, f)])

    def link_side(s):
        for name, target in (('ocamlc', s.byte), ('ocamlopt', s.opt)):
            lnk = os.path.join(bindir, name)
            rm([lnk])
            if target:
                os.symlink(target, lnk)
        return Side(s.name, os.path.join(bindir, 'ocamlc'),
                    os.path.join(bindir, 'ocamlopt'), s.flags)

    def steps_for(level, s):
        """[(cmd, run?)] and the artifacts to compare."""
        st, arts = [], []
        names = [u for u, _ in order]
        for u in mlionly:  # interface-only units: their .cmi (and .cmti)
            comp = s.byte if level != 'mfl' else s.opt
            st.append(([comp] + s.flags + W + ['-c', '-bin-annot', u + '.mli'], False))
            arts += [u + '.cmi', u + '.cmti']
        if level == 'mbyte':
            for u, mli in order:
                st.append(([s.byte] + s.flags + W + ['-i', u + '.ml'], False))
                if mli:
                    st.append(([s.byte] + s.flags + W + ['-c', '-bin-annot', u + '.mli'], False))
                    arts.append(u + '.cmti')
                st.append(([s.byte] + s.flags + W + ['-c', '-bin-annot', u + '.ml'], False))
                arts += [u + '.cmi', u + '.cmo', u + '.cmt']
            st.append(([s.byte] + s.flags + W + [u + '.cmo' for u in names] +
                       ['-o', 'mf.byte'], False))
            st.append(([os.path.join(ROOT, 'runtime/ocamlrun'), './mf.byte'], True))
            arts.append('mf.byte')
        elif level in ('mnat', 'mfl'):
            O = ['-O3'] if level == 'mfl' else []
            for u, mli in order:
                if mli:
                    st.append(([s.opt] + s.flags + W + O + ['-c', u + '.mli'], False))
                st.append(([s.opt] + s.flags + W + O + ['-c', '-S', u + '.ml'], False))
                arts += [u + '.cmi', u + '.cmx', u + '.o', u + '.s']
            st.append(([s.opt] + s.flags + W + O + [u + '.cmx' for u in names] +
                       ['-o', 'mf.nat'], False))
            st.append((['./mf.nat'], True))
            arts.append('mf.nat')
        elif level == 'mpack':
            for comp, ext in ((s.byte, '.cmo'), (s.opt, '.cmx')):
                for u, mli in order:
                    if mli:
                        st.append(([comp] + s.flags + W + ['-c', u + '.mli'], False))
                    st.append(([comp] + s.flags + W + ['-for-pack', 'Pk', '-c', u + '.ml'],
                               False))
                    arts.append(u + ext)
                st.append(([comp] + s.flags + W + ['-pack', '-o', 'pk' + ext] +
                           [u + ext for u in names], False))
                exe = 'pk.byte' if ext == '.cmo' else 'pk.nat'
                st.append(([comp] + s.flags + W + ['pk' + ext, '-o', exe], False))
                st.append(((([os.path.join(ROOT, 'runtime/ocamlrun')] if ext == '.cmo' else [])
                            + ['./' + exe]), True))
                arts += ['pk' + ext, 'pk.cmi', exe]
            arts += ['pk.o']
        elif level == 'mlib':
            for comp, ext, lib in ((s.byte, '.cmo', 'lib.cma'), (s.opt, '.cmx', 'lib.cmxa')):
                for u, mli in order:
                    if mli:
                        st.append(([comp] + s.flags + W + ['-c', u + '.mli'], False))
                    st.append(([comp] + s.flags + W + ['-c', u + '.ml'], False))
                st.append(([comp] + s.flags + W + ['-a', '-o', lib] +
                           [u + ext for u in names], False))
                exe = 'lib.byte' if ext == '.cmo' else 'lib.nat'
                st.append(([comp] + s.flags + W + ['-linkall', lib, '-o', exe], False))
                st.append(((([os.path.join(ROOT, 'runtime/ocamlrun')] if ext == '.cmo' else [])
                            + ['./' + exe]), True))
                arts += [lib, exe]
            arts += ['lib.a']
        return st, list(dict.fromkeys(arts))

    def run_side(level, s):
        st, arts = steps_for(level, s)
        log = []
        runs = []
        for cmd, is_run in st:
            r = run(cmd, wd, RUN_TIMEOUT if is_run else COMPILE_TIMEOUT)
            if is_run:
                runs.append(r)
            else:
                log.append(r)
            if not is_run and rc_of(r) != 0 and '-i' not in cmd:
                break
        a = {x: readb(os.path.join(wd, x)) for x in arts}
        clean()
        return b'\n'.join(log), a, runs

    clean()
    for level in levels:
        if level == 'mwp':
            continue
        if level == 'mfl' and not os.path.isdir(FL):
            continue
        res = {}
        for s in (fl_sides() if level == 'mfl' else sides()):
            res[s.name] = run_side(level, link_side(s))
        (sl, sa, sr), (pl, pa, pr) = res['stock'], res['port']
        if sl != pl:
            findings.append((level, sl, pl))
        for x in sa:
            if sa[x] != pa[x]:
                if x.endswith(('.cmt', '.cmti')):
                    ds, dp = cmtdump(wd, sa[x]), cmtdump(wd, pa[x])
                    rm([os.path.join(wd, 'dump.cmt')])
                    if ds == dp:
                        notes.append('cmt-sharing')
                        continue
                    findings.append(('%s:%s' % (level, x), ds, dp))
                    continue
                findings.append(('%s:%s' % (level, x), sa[x], pa[x]))
        if sr != pr:
            findings.append(('%s:run' % level, b'\n'.join(sr), b'\n'.join(pr)))
        if sr:
            out[level] = sr
    ref = out.get('mbyte', [None])[0]
    if any(o == b'TIMEOUT' for v in out.values() for o in v):
        notes.append('timeout:run')
        ref = None
    for k in ('mnat', 'mpack', 'mlib', 'mfl'):
        for o in out.get(k, []):
            # a packed unit's exceptions print as Pk.U.E
            if ref is not None and o.replace(b'Pk.', b'') != ref:
                notes.append('UNSPEC-%s' % k)
                break
    if 'mwp' in levels and os.path.isdir(FL) and ref is not None:
        r = run([os.path.join(FL, 'cxx/build-release/c++ocamlopt'), '-nostdlib', '-I',
                 os.path.join(FL, 'stdlib'), '-I', os.path.join(FL, 'runtime')] + wp_flags(W) +
                ['-O3'] + [u + '.mli' for u in mlionly] +
                sum([[u + '.mli', u + '.ml'] if m else [u + '.ml']
                     for u, m in order], []) + ['-o', 'mf.wp'],
                wd, COMPILE_TIMEOUT * 3, env={'CPPCAML_WP': '1', 'TMPDIR': wd})
        if rc_of(r) != 0:
            findings.append(('mwp:build', b'', r))
        else:
            o = run(['./mf.wp'], wd, RUN_TIMEOUT)
            out['mwp'] = [o]
            if o != ref and not any(n.startswith('UNSPEC') for n in notes):
                findings.append(('mwp:run', ref, o))
        clean()
    return findings, notes, {k: v[0] for k, v in out.items()}


CMTDUMP = None


def cmtdump(wd, data):
    """cmt_parity.sh's structural dump (every cmt_infos field, the typed
    tree through Printtyped), to tell Marshal sharing from content."""
    p = os.path.join(wd, 'dump.cmt')
    with open(p, 'wb') as f:
        f.write(data)
    return run([os.path.join(ROOT, 'runtime/ocamlrun'), CMTDUMP, p], wd, 60)


def build_cmtdump(outdir):
    global CMTDUMP
    tools = os.path.join(os.path.abspath(outdir), 'tools')
    os.makedirs(tools, exist_ok=True)
    exe = os.path.join(tools, 'cmtdump')
    if not os.path.exists(exe):
        shutil.copy(os.path.join(ROOT, 'cxx/harness/cmt/cmtdump.ml'), tools)
        inc = []
        for d in ('stdlib', 'compilerlibs', 'utils', 'typing', 'parsing',
                  'file_formats', 'driver', 'lambda'):
            inc += ['-I', os.path.join(ROOT, d)]
        subprocess.run([os.path.join(ROOT, 'ocamlc.opt'), '-nostdlib'] + inc +
                       [os.path.join(ROOT, 'compilerlibs/ocamlcommon.cma'),
                        'cmtdump.ml', '-o', 'cmtdump'], cwd=tools, check=True)
    CMTDUMP = exe
    os.environ['FUZZ_CMTDUMP'] = exe


def split_flags(wflags):
    """-> (flags for every compiler, ocamlopt-only flags, flambda flags):
    a `nat:F` token is F for the closure ocamlopt only, `fl:F` for the
    flambda one only (default -O3)."""
    W = [f for f in wflags if not f.startswith(('nat:', 'fl:'))]
    NW = [f[4:] for f in wflags if f.startswith('nat:')]
    FW = [f[3:] for f in wflags if f.startswith('fl:')] or ['-O3']
    return W, NW, FW


def wp_flags(W):
    """Whole-program mode compiles the stdlib's sources with the program's
    flags: -unboxed-types would make them miss their own interfaces."""
    return [f for f in W if f != '-unboxed-types']


NAT_FLAGS = [[], ['-linscan'], ['-inline', '0'], ['-inline', '100', '-unsafe'],
             ['-no-float-const-prop'], ['-function-sections'], ['-compact'],
             ['-no-insn-sched', '-nodynlink'], ['-afl-instrument'], ['-g'],
             ['-unboxed-types', '-linscan'], []]
FL_FLAGS = [['-O3'], ['-O2'], ['-Oclassic'], ['-O3', '-unbox-closures'],
            ['-O3', '-remove-unused-arguments', '-inline-max-unroll', '3'],
            ['-O3', '-no-float-const-prop'], ['-O3', '-linscan', '-unsafe'],
            ['-O2', '-inline', '300', '-inline-max-depth', '3'], ['-O3', '-g'], ['-O3']]


def flags_for(seed):
    """The common flags (by seed % 11), then the back ends' own
    (`nat:`/`fl:` tokens, by seed // 11)."""
    k = seed // 11
    extra = ['nat:' + f for f in NAT_FLAGS[k % len(NAT_FLAGS)]] + \
        ['fl:' + f for f in FL_FLAGS[(k // len(NAT_FLAGS)) % len(FL_FLAGS)]]
    return common_flags_for(seed) + extra


def common_flags_for(seed):
    r = seed % 11
    if r == 1:
        return ['-unboxed-types']
    if r == 4:
        return ['-unsafe']
    if r == 3:
        return ['-w', '+a']
    if r == 5:
        return ['-principal']
    if r == 6:
        return ['-w', '+a-4-9-40-41-42-44-45', '-strict-sequence']
    if r == 7:
        return ['-short-paths']
    if r == 8:
        return ['-g']
    if r == 9:
        return ['-opaque']
    if r == 10:
        return ['-no-alias-deps', '-w', '+a']
    return []


def one_mf(seed, levels, outdir, ill=False):
    import genmf
    wd = os.path.join(outdir, 'work', str(seed))
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    W = flags_for(seed)
    try:
        genmf.build(seed, wd, ill)
    except Exception as ex:
        return seed, [('gen', b'', repr(ex).encode())], [], ' '.join(W)
    try:
        findings, notes, out = compare_mf(wd, levels, W)
    except Exception as ex:  # harness error: report, keep going
        return seed, [('harness', b'', repr(ex).encode())], [], ' '.join(W)
    if findings:
        for level, a, b in findings:
            fd = os.path.join(outdir, 'findings', '%d-%s' % (seed, level.replace(':', '_')))
            shutil.rmtree(fd, ignore_errors=True)
            shutil.copytree(wd, fd, ignore=shutil.ignore_patterns('.bin'))
            with open(os.path.join(fd, 'stock'), 'wb') as f:
                f.write(a)
            with open(os.path.join(fd, 'port'), 'wb') as f:
                f.write(b)
            with open(os.path.join(fd, 'info'), 'w') as f:
                f.write('seed %d level %s mf flags %s\n' % (seed, level, ' '.join(W)))
    shutil.rmtree(wd, ignore_errors=True)
    return seed, findings, notes, ' '.join(W)


def one(seed, levels, outdir, ill, mf=False):
    global CMTDUMP
    CMTDUMP = os.environ.get('FUZZ_CMTDUMP')
    if mf:
        return one_mf(seed, levels, outdir, ill)
    wd = os.path.join(outdir, 'work', str(seed))
    os.makedirs(wd, exist_ok=True)
    g = gen.Gen(seed, ill)
    marked = g.program()
    nodes = ' '.join('%d=%s' % (k, v.encode().hex()) for k, v in g.nodes.items())
    src = gen.strip(marked)
    with open(os.path.join(wd, 'fz.ml'), 'w') as f:
        f.write(src)
    W = flags_for(seed)
    try:
        findings, notes, out = compare_levels(wd, levels, W)
    except Exception as ex:  # harness error: report, keep going
        return seed, [('harness', b'', repr(ex).encode())], [], ''
    if 'stock-rejects' in notes and not ill:
        # well-typed mode: stock rejecting the program is a generator bug
        r = run([os.path.join(ROOT, 'ocamlc.opt'), '-nostdlib', '-I',
                 os.path.join(ROOT, 'stdlib')] + split_flags(W)[0] + ['-i', 'fz.ml'], wd, COMPILE_TIMEOUT)
        findings.append(('gen:stock-rejects', r, b''))
    timeouts = [k for k, v in out.items() if v == b'TIMEOUT']
    if timeouts:
        notes.append('timeout:' + ','.join(timeouts))
    if findings:
        for level, a, b in findings:
            fd = os.path.join(outdir, 'findings', '%d-%s' % (seed, level.replace(':', '_')))
            os.makedirs(fd, exist_ok=True)
            with open(os.path.join(fd, 'fz.ml'), 'w') as f:
                f.write(src)
            with open(os.path.join(fd, 'fz.marked'), 'w') as f:
                f.write(marked + '(*NODES %s*)\n' % nodes)
            with open(os.path.join(fd, 'stock'), 'wb') as f:
                f.write(a)
            with open(os.path.join(fd, 'port'), 'wb') as f:
                f.write(b)
            with open(os.path.join(fd, 'info'), 'w') as f:
                f.write('seed %d level %s ill %s flags %s\n' % (seed, level, ill, ' '.join(W)))
    elif any(n.startswith('UNSPEC') for n in notes):
        fd = os.path.join(outdir, 'unspec', str(seed))
        os.makedirs(fd, exist_ok=True)
        shutil.copy(os.path.join(wd, 'fz.ml'), fd)
        for k, v in out.items():
            with open(os.path.join(fd, 'run.' + k), 'wb') as f:
                f.write(v or b'')
    shutil.rmtree(wd, ignore_errors=True)
    return seed, findings, notes, ' '.join(W)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--start', type=int, default=1)
    ap.add_argument('--count', type=int, default=100)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--ill', action='store_true')
    ap.add_argument('--mf', action='store_true', help='multi-file programs (genmf.py)')
    ap.add_argument('--levels', default=None)
    ap.add_argument('--out', default='fuzz-out')
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)  # the compilers run inside work dirs
    levels = (a.levels or ','.join(MF_LEVELS if a.mf else ALL_LEVELS)).split(',')
    if a.ill:
        levels = [l for l in levels if l in ('i', 'bc', 'nat', 'mbyte')]
    os.makedirs(a.out, exist_ok=True)
    build_cmtdump(a.out)
    seeds = range(a.start, a.start + a.count)
    stats = {'programs': 0, 'clean': 0, 'finding': 0, 'stock-rejects': 0,
             'unspec': 0, 'timeout': 0}
    log = open(os.path.join(a.out, 'log'), 'a')
    with cf.ProcessPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(one, s, levels, a.out, a.ill, a.mf) for s in seeds]
        for fu in cf.as_completed(futs):
            seed, findings, notes, W = fu.result()
            stats['programs'] += 1
            if 'stock-rejects' in notes:
                stats['stock-rejects'] += 1
            if any(n.startswith('UNSPEC') for n in notes):
                stats['unspec'] += 1
            if any(n.startswith('timeout') or n == 'wp-timeout' for n in notes):
                stats['timeout'] += 1
            if findings:
                stats['finding'] += 1
                lv = sorted(set(l for l, _, _ in findings))
                print('FINDING seed %d %s %s' % (seed, ' '.join(lv), W), flush=True)
            elif not notes:
                stats['clean'] += 1
            log.write('%d %s %s %s\n' % (seed, 'F:' + ','.join(sorted(set(
                l for l, _, _ in findings))) if findings else 'ok', ' '.join(notes), W))
            log.flush()
    print(' '.join('%s=%d' % kv for kv in stats.items()))


if __name__ == '__main__':
    main()
