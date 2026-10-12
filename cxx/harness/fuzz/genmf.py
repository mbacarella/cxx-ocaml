#!/usr/bin/env python3
"""Random multi-file programs for differential testing (fuzz.py --mf).

    genmf.py SEED DIR

gen.py's program for SEED, split into compilation units: pre.ml (the
prelude) and u1.ml .. uK.ml, each opening the units before it, so the
program's toplevel effects run in the same order when the units are
linked in DIR/order.  Most units get an interface: stock `ocamlc -i` of
the unit, then a few random edits, each kept only when stock still
compiles every unit (abstract a type, make it private, drop a value,
replace a module's signature by `sig include S end`, re-export a type of
an earlier unit through a manifest `type t = U.t = ...`).  The stock
compiler is the tree's ocamlc.opt (ROOT, as in fuzz.py).

DIR/order lists the units in link order, one per line, a unit with an
interface followed by " mli", an interface-only unit (sigs.mli, module
types and a type, no implementation) by " mlionly".
"""
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('ROOT', os.path.abspath(os.path.join(HERE, '..', '..', '..')))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # (no __pycache__ in the tree)
import gen  # noqa: E402


def stock(args, wd):
    return subprocess.run([os.path.join(ROOT, 'ocamlc.opt'), '-nostdlib', '-I',
                           os.path.join(ROOT, 'stdlib'), '-w', '-a'] + args, cwd=wd,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)


def items_of(mli):
    """Split -i output into toplevel items (a column-0 line with its
    indented continuation lines; `and` lines stay with their group)."""
    items = []
    for line in mli.split('\n'):
        if not line:
            continue
        if items and (line[0] in ' }])' or line.startswith('and ') or line == 'end'
                      or line.startswith('end ')):
            items[-1] += '\n' + line
        else:
            items.append(line)
    return items


TYPE_RE = re.compile(r"^type ((?:'\w+ |\([^)]*\) )?)(\w+) = (.*)$", re.S)


def mutations(items, unit, prev_types):
    """Candidate (index, new item) edits of one interface."""
    out = []
    for i, it in enumerate(items):
        if it.startswith('type ') and '\nand ' not in it:
            m = TYPE_RE.match(it)
            if m:
                params, name, rhs = m.groups()
                out.append((i, 'type %s%s' % (params, name)))
                if not rhs.startswith('private'):
                    out.append((i, 'type %s%s = private %s' % (params, name, rhs)))
        elif it.startswith('val ') or it.startswith('external '):
            out.append((i, None))
        elif re.match(r'^module (\w+) : (\w+)$', it):
            m = re.match(r'^module (\w+) : (\w+)$', it)
            out.append((i, 'module %s : sig include %s end' % m.groups()))
        elif it.startswith('class ') or it.startswith('exception '):
            if '\nand ' not in it and not it.startswith('class type'):
                out.append((i, None))
    return out


def abstraction_unit(r):
    """absu.ml with a hand-written interface that hides or exposes its
    types at random (abstract, private, manifest), and the uses of it the
    last unit makes, each allowed by what the interface exposes."""
    k = r.randint(1, 9)
    ml = ('type t = { mutable n : int; s : string }\n'
          'type p = A of int | B of string\n'
          'type q = int\n'
          'let make n = { n = n * %d; s = string_of_int n }\n'
          'let get t = t.n + String.length t.s\n'
          'let bump t = t.n <- t.n + 1; t\n'
          'let pa n = if n land 1 = 0 then A n else B (string_of_int n)\n'
          'let show_p = function A n -> "A" ^ string_of_int n | B s -> "B" ^ s\n'
          'let q n : q = n * 3\n'
          'let show_q = string_of_int\n' % k)
    tk = r.choice(['abs', 'priv', 'man'])
    pk = r.choice(['abs', 'priv', 'man'])
    qk = r.choice(['abs', 'priv', 'man'])
    tdef = {'abs': 'type t', 'priv': 'type t = private { mutable n : int; s : string }',
            'man': 'type t = { mutable n : int; s : string }'}[tk]
    pdef = {'abs': 'type p', 'priv': 'type p = private A of int | B of string',
            'man': 'type p = A of int | B of string'}[pk]
    qdef = {'abs': 'type q', 'priv': 'type q = private int', 'man': 'type q = int'}[qk]
    vals = ['val make : int -> t', 'val get : t -> int', 'val bump : t -> t',
            'val pa : int -> p', 'val show_p : p -> string', 'val q : int -> q',
            'val show_q : q -> string']
    if r.random() < 0.5:
        mli = '\n'.join([tdef, pdef, qdef] + vals) + '\n'
    else:
        # the same through an include of a named signature
        sig = ('module type ABS = sig\n  %s\n  %s\n  %s\n  %s\nend\n'
               % (tdef, pdef, qdef, '\n  '.join(vals)))
        mli = sig + 'include ABS\n'
        ml = sig + ml
    uses = ['string_of_int (Absu.get (Absu.bump (Absu.make 5)))',
            'Absu.show_p (Absu.pa 3)', 'Absu.show_q (Absu.q 7)']
    if tk != 'abs':
        uses.append('string_of_int (Absu.make 4).Absu.n')
    if tk == 'man':
        uses.append('string_of_int (Absu.get { Absu.n = 2; s = "xy" })')
    if pk != 'abs':
        uses.append('(match Absu.pa 4 with Absu.A n -> string_of_int n | Absu.B s -> s)')
    if pk == 'man':
        uses.append('Absu.show_p (Absu.B "b")')
    if qk == 'priv':
        uses.append('string_of_int ((Absu.q 7 :> int) + 1)')
    if qk == 'man':
        uses.append('string_of_int (Absu.q 7 + 1)')
    use = 'let () = print_endline ("absu:" ^ String.concat "," [%s])\n' % '; '.join(uses)
    return ml, mli, use


def ill_edit(r, names, has_mli, mlis, wd):
    """One interface edit that (usually) breaks the program, not checked:
    for comparing the error messages of signature matching and of the
    units after it."""
    cands = []
    for n in names:
        if not has_mli[n]:
            continue
        its = items_of(mlis[n])
        for i, it in enumerate(its):
            if it.startswith('val '):
                head, _, ty = it.partition(' : ')
                if re.search(r'\bint\b', ty):
                    cands.append((n, i, head + ' : ' + re.sub(r'\bint\b', 'string', ty, 1)))
                else:
                    cands.append((n, i, head + ' : (' + ty + ') list'))
                cands.append((n, i, None))
            m = TYPE_RE.match(it)
            if m and '\nand ' not in it:
                params, name, rhs = m.groups()
                cands.append((n, i, 'type %s%s' % (params, name)))
                if 'mutable ' in rhs:
                    cands.append((n, i, it.replace('mutable ', '', 1)))
                fs = re.match(r'^\{ ([^;{}]+); ([^;{}]+);(.*)$', rhs, re.S)
                if fs:
                    cands.append((n, i, 'type %s%s = { %s; %s;%s' % (
                        params, name, fs.group(2), fs.group(1), fs.group(3))))
                cs = re.match(r'^\s*\|?\s*([A-Z]\w*(?: of [^|]*)?)\s*\|\s*([A-Z]\w*(?: of [^|]*)?)(.*)$',
                              rhs, re.S)
                if cs:
                    cands.append((n, i, 'type %s%s = %s | %s%s' % (
                        params, name, cs.group(2).strip(), cs.group(1).strip(), cs.group(3))))
            mm = re.match(r'^module (\w+) : (\w+)$', it)
            if mm:
                cands.append((n, i, 'module %s : %s with type t = string' % mm.groups()))
        cands.append((n, len(its), 'val ill_missing : int'))
        cands.append((n, len(its), 'type ill_t'))
    if not cands:
        return
    n, i, new = r.choice(cands)
    its = items_of(mlis[n])
    if i == len(its):
        its.append(new)
    elif new is None:
        del its[i]
    else:
        its[i] = new
    with open(os.path.join(wd, n + '.mli'), 'w') as f:
        f.write('\n'.join(its) + '\n')


def build(seed, wd, ill=False):
    r = random.Random(seed * 31 + 5)
    g = gen.Gen(seed)
    g.program()
    items = [gen.strip(x) for x in g.items]
    prelude, body = items[0], items[1:]
    k = r.randint(2, min(5, max(2, len(body))))
    cuts = sorted(r.sample(range(1, len(body)), k - 1)) if len(body) > k else list(range(1, k))
    chunks = [body[a:b] for a, b in zip([0] + cuts, cuts + [len(body)])]
    units = [('pre', prelude)]
    absu, absu_mli, absu_use = abstraction_unit(r)
    for j, ch in enumerate(chunks):
        name = 'u%d' % (j + 1)
        opens = ''.join('open! %s\n' % n.capitalize() for n, _ in units if n != 'absu')
        units.append((name, opens + '\n'.join(ch) + '\n'))
        if j == 0:
            units.append(('absu', absu))
    units[-1] = (units[-1][0], units[-1][1] + absu_use)
    sigs = r.random() < 0.5
    if sigs:
        # an interface-only unit: module types, used through its .cmi alone
        k = r.randint(0, 99)
        with open(os.path.join(wd, 'sigs.mli'), 'w') as f:
            f.write('module type SX = sig type t val v : t val show : t -> string end\n'
                    "type 'a pair = { l : 'a; r : 'a }\n"
                    'module type SP = sig include SX val p : t pair end\n')
        stock(['-c', 'sigs.mli'], wd)
        units[-1] = (units[-1][0], units[-1][1] +
                     'module Sx : Sigs.SP = struct type t = int let v = %d '
                     'let show = string_of_int let p = { Sigs.l = v; r = v + 1 } end\n'
                     'let () = print_endline ("sx=" ^ Sx.show Sx.v ^ Sx.show Sx.p.Sigs.r)\n' % k)
    for n, src in units:
        with open(os.path.join(wd, n + '.ml'), 'w') as f:
            f.write(src)
    has_mli = {n: r.random() < (0.3 if n == 'pre' else 0.7) for n, _ in units}
    has_mli['absu'] = True
    mlis = {}
    # the unedited interfaces: -i of each unit, compiling in order
    for idx, (n, _) in enumerate(units):
        extra = []
        if idx > 0 and n != 'absu' and r.random() < 0.5:
            # re-export a type of an earlier unit through a manifest
            reex = []
            for m, _ in units[:idx]:
                if m in mlis:
                    for it in items_of(mlis[m]):
                        tm = TYPE_RE.match(it)
                        if tm and '\nand ' not in it and re.match(
                                r'(\{|\||[A-Z]\w*(\s+of\b|\s*\||\s*$))', tm.group(3).lstrip()):
                            reex.append((m, tm.groups()))
            if reex:
                m, (params, tname, rhs) = r.choice(reex)
                extra.append('type %s%s_re = %s%s.%s = %s' % (
                    params, tname, params, m.capitalize(), tname, rhs))
        if idx > 1 and n != 'absu' and r.random() < 0.3:
            extra.append('module Inc%d = struct include %s end' % (
                idx, r.choice([m for m, _ in units[1:idx] if m != 'absu']).capitalize()))
        if extra:
            with open(os.path.join(wd, n + '.ml'), 'a') as f:
                f.write('\n'.join(extra) + '\n')
        if n == 'absu':
            mlis[n] = absu_mli
            with open(os.path.join(wd, n + '.mli'), 'w') as f:
                f.write(absu_mli)
            stock(['-c', n + '.mli'], wd)
        elif has_mli[n]:
            p = stock(['-i', n + '.ml'], wd)
            if p.returncode != 0:
                raise RuntimeError('stock -i failed on %s: %s' % (n, p.stderr.decode()))
            mlis[n] = p.stdout.decode()
            with open(os.path.join(wd, n + '.mli'), 'w') as f:
                f.write(mlis[n])
            stock(['-c', n + '.mli'], wd)
        p = stock(['-c', n + '.ml'], wd)
        if p.returncode != 0:
            raise RuntimeError('stock failed on %s: %s' % (n, p.stderr.decode()))
    names = [n for n, _ in units]

    def rebuild_from(idx):
        for n in names[idx:]:
            if has_mli[n] and stock(['-c', n + '.mli'], wd).returncode != 0:
                return False
            if stock(['-c', n + '.ml'], wd).returncode != 0:
                return False
        return True

    # edits, each kept only if stock still compiles the program
    tries = r.randint(4, 12)
    for _ in range(tries):
        cands = [n for n in names if has_mli[n] and n != 'absu']
        if not cands:
            break
        n = r.choice(cands)
        its = items_of(mlis[n])
        ms = mutations(its, n, None)
        if not ms:
            continue
        i, new = r.choice(ms)
        its2 = list(its)
        if new is None:
            del its2[i]
        else:
            its2[i] = new
        text = '\n'.join(its2) + '\n'
        with open(os.path.join(wd, n + '.mli'), 'w') as f:
            f.write(text)
        if rebuild_from(names.index(n)):
            mlis[n] = text
        else:
            with open(os.path.join(wd, n + '.mli'), 'w') as f:
                f.write(mlis[n])
            rebuild_from(names.index(n))
    if ill:
        ill_edit(r, names, has_mli, mlis, wd)
    # leave only sources behind
    for f in os.listdir(wd):
        if f.endswith(('.cmi', '.cmo', '.cmt', '.cmti')):
            os.unlink(os.path.join(wd, f))
    with open(os.path.join(wd, 'order'), 'w') as f:
        if sigs:
            f.write('sigs mlionly\n')
        for n in names:
            f.write(n + (' mli' if has_mli[n] else '') + '\n')
    return names, has_mli


def main():
    seed = int(sys.argv[1])
    wd = sys.argv[2]
    os.makedirs(wd, exist_ok=True)
    build(seed, wd)


if __name__ == '__main__':
    main()
