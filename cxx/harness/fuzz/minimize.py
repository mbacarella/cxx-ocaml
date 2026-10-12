#!/usr/bin/env python3
"""Shrink a fuzz.py finding while it keeps differing at the same level.

    minimize.py FINDING_DIR [--max-tests N]

FINDING_DIR is fuzz.py's findings/SEED-LEVEL directory (fz.marked, info).
Two passes, repeated to a fixpoint:
  1. drop whole toplevel items (a line starting with let/type/module/
     class/exception at column 0, with its continuation lines);
  2. replace a generator node (\\x01ID\\x02 ... \\x03) by the closed constant
     of its type the generator recorded, largest nodes first.
A candidate is kept when the same level still differs between stock and
port (for a whole-program run, the stock bytecode output is the reference).
Writes FINDING_DIR/min.ml.
"""
import argparse
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # (no __pycache__ in the tree)
import fuzz  # noqa: E402
import gen  # noqa: E402

LEVELS = {'i': ['i'], 'bc': ['bc'], 'bcg': ['bcg'], 'byte': ['byte'],
          'nat': ['nat'], 'natexe': ['nat'], 'fl': ['fl'], 'flexe': ['fl'],
          'wp': ['byte', 'nat', 'wp']}


def parse_nodes(text):
    m = re.search(r'\(\*NODES (.*)\*\)\n?$', text)
    nodes = {}
    if m:
        for kv in m.group(1).split():
            k, v = kv.split('=')
            nodes[int(k)] = bytes.fromhex(v).decode()
        text = text[:m.start()]
    return text, nodes


def spans(text):
    """node id -> (start, end) of the whole marked span."""
    out = {}
    stack = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == gen.MARK_OPEN:
            j = text.index(gen.MARK_MID, i)
            stack.append((int(text[i + 1:j]), i))
            i = j + 1
        elif c == gen.MARK_CLOSE:
            nid, st = stack.pop()
            out[nid] = (st, i + 1)
            i += 1
        else:
            i += 1
    return out


def items(text):
    lines = text.split('\n')
    starts = [i for i, l in enumerate(lines)
              if re.match(r'(let|type|module|class|exception)\b', l)]
    groups = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else len(lines)
        groups.append((s, e))
    return lines, groups


class Pred:
    def __init__(self, fd, level, flags, max_tests):
        self.wd = os.path.join(fd, 'mwork')
        self.level = level
        self.base = level.split(':')[0]
        self.flags = flags
        self.tests = 0
        self.max_tests = max_tests

    def __call__(self, marked):
        if self.tests >= self.max_tests:
            return False
        self.tests += 1
        shutil.rmtree(self.wd, ignore_errors=True)
        os.makedirs(self.wd)
        with open(os.path.join(self.wd, 'fz.ml'), 'w') as f:
            f.write(gen.strip(marked))
        if self.base.startswith('x'):
            # stock against stock: bytecode vs the back end named after x
            # (xnat, xfl), e.g. to reduce a stock flambda miscompilation
            be = self.base[1:]
            findings, notes, out = fuzz.compare_levels(
                self.wd, ['byte', be], self.flags)
            return out.get('byte') not in (None, b'TIMEOUT') and \
                out.get(be) not in (None, b'TIMEOUT') and \
                out['byte'] != out[be]
        findings, notes, out = fuzz.compare_levels(
            self.wd, LEVELS.get(self.base, [self.base]), self.flags)
        if any(n.startswith('UNSPEC') for n in notes):
            return False
        # a whole-program finding stays one about a program stock compiles
        # and runs (a type error would fail the whole-program build too)
        if self.base == 'wp' and out.get('byte') in (None, b'TIMEOUT'):
            return False
        return any(l == self.level for l, _, _ in findings)


def minimize(fd, max_tests):
    with open(os.path.join(fd, 'info')) as f:
        info = f.read().split()
    level = info[info.index('level') + 1]
    fl = info[info.index('flags') + 1:] if 'flags' in info else []
    with open(os.path.join(fd, 'fz.marked')) as f:
        text, nodes = parse_nodes(f.read())
    fuzz.build_cmtdump(os.path.join(fd, '..', '..'))
    pred = Pred(fd, level, fl, max_tests)
    if not pred(text):
        print('%s: does not reproduce' % fd)
        return None
    changed = True
    while changed and pred.tests < max_tests:
        changed = False
        # pass 1: toplevel items, last first (later items use earlier ones)
        lines, groups = items(text)
        for s, e in reversed(groups):
            if s < 1:
                continue
            cand = '\n'.join(lines[:s] + lines[e:])
            if pred(cand):
                text = cand
                lines, groups = items(text)
                changed = True
        # pass 2: nodes, largest first
        sp = spans(text)
        order = sorted(sp, key=lambda k: sp[k][0] - sp[k][1])
        done = set()
        for nid in order:
            if nid not in sp or nid in done or nid not in nodes:
                continue
            st, en = sp[nid]
            repl = nodes[nid]
            if gen.strip(text[st:en]) == repl:
                continue
            cand = text[:st] + repl + text[en:]
            done.add(nid)
            if pred(cand):
                text = cand
                sp = spans(text)
                changed = True
    out = gen.strip(text)
    with open(os.path.join(fd, 'min.ml'), 'w') as f:
        f.write(out)
    shutil.rmtree(pred.wd, ignore_errors=True)
    print('%s: %d tests, %d -> %d lines' % (fd, pred.tests, len(
        gen.strip(parse_nodes(open(os.path.join(fd, 'fz.marked')).read())[0]).split('\n')),
        len(out.split('\n'))))
    return out


def from_seed(outdir, seed, level, ill=False):
    """A finding directory for SEED at LEVEL, regenerated (gen.py is
    deterministic)."""
    fd = os.path.join(outdir, 'findings', '%d-%s' % (seed, level.replace(':', '_')))
    os.makedirs(fd, exist_ok=True)
    g = gen.Gen(seed, ill)
    marked = g.program()
    nodes = ' '.join('%d=%s' % (k, v.encode().hex()) for k, v in g.nodes.items())
    with open(os.path.join(fd, 'fz.marked'), 'w') as f:
        f.write(marked + '(*NODES %s*)\n' % nodes)
    with open(os.path.join(fd, 'info'), 'w') as f:
        f.write('seed %d level %s ill %s flags %s\n' % (seed, level, ill,
                                                       ' '.join(fuzz.flags_for(seed))))
    return fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='*')
    ap.add_argument('--max-tests', type=int, default=600)
    ap.add_argument('--seed', type=int, action='append', default=[],
                    help='regenerate SEED into --out/findings (with --level)')
    ap.add_argument('--level', default='xfl')
    ap.add_argument('--ill', action='store_true')
    ap.add_argument('--out', default='fuzz-min')
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)  # the compilers run inside work dirs
    dirs = [os.path.abspath(d) for d in a.dirs] + \
        [from_seed(a.out, s, a.level, a.ill) for s in a.seed]
    for d in dirs:
        minimize(d, a.max_tests)


if __name__ == '__main__':
    main()
