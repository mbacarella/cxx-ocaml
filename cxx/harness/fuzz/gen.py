#!/usr/bin/env python3
"""Random well-typed OCaml programs for differential testing (fuzz.sh).

    gen.py SEED [--ill] [--marked]

Prints one compilation unit.  The program is deterministic in SEED, always
terminates, and prints a digest of every value it computes, so its output can
be compared across compilers and back ends.  Evaluation order is never left
to the compiler: whenever two operands of one construct (application,
tuple, constructor, record, primitive) have effects -- raise, mutation,
reading mutable state, output -- they are let-bound in left-to-right order
first.  Physical equality, int_of_float of arbitrary floats and other
unspecified operations are not generated.

--ill turns exactly one generated expression into an ill-typed one (wrong
type, unbound name or wrong label), for comparing error messages.

--marked keeps the node markers the minimizer (minimize.py) works on:
\\x01ID\\x02 code \\x03, each node recorded with a closed constant of its type
in a trailer line "(*NODES ID=const...*)".
"""
import random
import sys

INT = ('int',)
BOOL = ('bool',)
STRING = ('string',)
FLOAT = ('float',)
CHAR = ('char',)
UNIT = ('unit',)
BASE = [INT, BOOL, STRING, FLOAT, CHAR]


def arrow(a, b, lbl=''):
    return ('arrow', lbl, a, b)


class E:
    """An expression: code, whether evaluating it has effects, whether
    calling it (arrow types) has effects."""
    __slots__ = ('code', 'eff', 'ceff')

    def __init__(self, code, eff=False, ceff=False):
        self.code = code
        self.eff = eff
        self.ceff = ceff


class Var:
    __slots__ = ('name', 'ty', 'ceff', 'cost')

    def __init__(self, name, ty, ceff=False, cost=1):
        self.name = name
        self.ty = ty
        self.ceff = ceff
        self.cost = cost


class Poly:
    """A polymorphic value: ty uses ('tv', n)."""
    __slots__ = ('name', 'ty', 'calls_args')

    def __init__(self, name, ty, calls_args=False):
        self.name = name
        self.ty = ty
        self.calls_args = calls_args


def subst(t, s):
    if t[0] == 'tv':
        return s[t[1]]
    if t[0] in ('list', 'option', 'ref', 'array', 'lazy', 'box'):
        return (t[0], subst(t[1], s))
    if t[0] == 'tuple':
        return ('tuple', tuple(subst(x, s) for x in t[1]))
    if t[0] == 'arrow':
        return ('arrow', t[1], subst(t[2], s), subst(t[3], s))
    return t


def match_ty(p, t, s):
    """One-way match of scheme p against ground t, extending s."""
    if p[0] == 'tv':
        if p[1] in s:
            return s[p[1]] == t
        s[p[1]] = t
        return True
    if p[0] != t[0]:
        return False
    if p[0] in ('list', 'option', 'ref', 'array', 'lazy', 'box'):
        return match_ty(p[1], t[1], s)
    if p[0] == 'tuple':
        return len(p[1]) == len(t[1]) and all(
            match_ty(a, b, s) for a, b in zip(p[1], t[1]))
    if p[0] == 'arrow':
        return p[1] == t[1] and match_ty(p[2], t[2], s) and \
            match_ty(p[3], t[3], s)
    return p == t


def tvs(t, acc):
    if t[0] == 'tv':
        acc.add(t[1])
    elif t[0] in ('list', 'option', 'ref', 'array', 'lazy', 'box'):
        tvs(t[1], acc)
    elif t[0] == 'tuple':
        for x in t[1]:
            tvs(x, acc)
    elif t[0] == 'arrow':
        tvs(t[2], acc)
        tvs(t[3], acc)
    return acc


TV = [('tv', i) for i in range(4)]
# Polymorphic helpers defined in the prelude, with their schemes.
PRELUDE_POLYS = [
    Poly('pid', arrow(TV[0], TV[0])),
    Poly('pconst', arrow(TV[0], arrow(TV[1], TV[0]))),
    Poly('pswap', arrow(('tuple', (TV[0], TV[1])), ('tuple', (TV[1], TV[0])))),
    Poly('papply', arrow(arrow(TV[0], TV[1]), arrow(TV[0], TV[1])), True),
    Poly('pcompose', arrow(arrow(TV[1], TV[2]),
                           arrow(arrow(TV[0], TV[1]), arrow(TV[0], TV[2]))), True),
    Poly('pdup', arrow(TV[0], ('tuple', (TV[0], TV[0])))),
    Poly('pmaybe', arrow(TV[0], arrow(('option', TV[0]), TV[0]))),
    Poly('List.length', arrow(('list', TV[0]), INT)),
    Poly('List.rev', arrow(('list', TV[0]), ('list', TV[0]))),
    Poly('List.map', arrow(arrow(TV[0], TV[1]),
                           arrow(('list', TV[0]), ('list', TV[1]))), True),
    Poly('List.filter', arrow(arrow(TV[0], BOOL),
                              arrow(('list', TV[0]), ('list', TV[0]))), True),
    Poly('List.fold_left', arrow(arrow(TV[0], arrow(TV[1], TV[0])),
                                 arrow(TV[0], arrow(('list', TV[1]), TV[0]))), True),
    Poly('pfirst', arrow(('list', TV[0]), ('option', TV[0]))),
    Poly('Option.map', arrow(arrow(TV[0], TV[1]),
                             arrow(('option', TV[0]), ('option', TV[1]))), True),
    Poly('Array.to_list', arrow(('array', TV[0]), ('list', TV[0]))),
    Poly('Array.length', arrow(('array', TV[0]), INT)),
    Poly('pbox', arrow(TV[0], ('box', TV[0]))),
    Poly('punbox', arrow(TV[0], arrow(('box', TV[0]), TV[0]))),
    Poly('Lazy.force', arrow(('lazy', TV[0]), TV[0]), True),
]

PRELUDE = r'''let show_list f l = "[" ^ String.concat "; " (List.map f l) ^ "]"
let show_array f a = "[|" ^ String.concat "; " (Array.to_list (Array.map f a)) ^ "|]"
let show_option f = function None -> "None" | Some x -> "Some(" ^ f x ^ ")"
let show_string s = "\"" ^ String.escaped s ^ "\""
let show_char c = "'" ^ Char.escaped c ^ "'"
let show_float f = Printf.sprintf "%h" f
let show_ref f r = "ref(" ^ f !r ^ ")"
let show_lazy f l = "lazy(" ^ f (Lazy.force l) ^ ")"
let show_fun _ = "<fun>"
let show_unit () = "()"
let cap s = if String.length s > 64 then String.sub s 0 64 else s
let pid x = x
let pconst x _ = x
let pswap (a, b) = (b, a)
let papply f x = f x
let pcompose f g x = f (g x)
let pdup x = (x, x)
let pmaybe d = function None -> d | Some x -> x
let pfirst = function [] -> None | x :: _ -> Some x
type 'a box = Box of 'a | Pair of 'a * 'a | Empty
let pbox x = Box x
let punbox d = function Box x -> x | Pair (_, y) -> y | Empty -> d
let show_box f = function
  | Box x -> "Box(" ^ f x ^ ")"
  | Pair (a, b) -> "Pair(" ^ f a ^ "," ^ f b ^ ")"
  | Empty -> "Empty"
let safe_div a b = a / (if b = 0 then 1 else b)
let ( #+ ) a b = a + 2 * b
let ( #^ ) a b = a ^ "#" ^ b
let gr = ref 0
let gs = ref ""
let ga = Array.make 8 0
let gh : (int, string) Hashtbl.t = Hashtbl.create 16
let gb = Buffer.create 64
'''

MARK_OPEN, MARK_MID, MARK_CLOSE = '\x01', '\x02', '\x03'


class Budget(Exception):
    pass


class Gen:
    def __init__(self, seed, ill=False):
        self.r = random.Random(seed)
        self.r2 = random.Random(seed * 7919 + 17)  # second-round features
        self.n = 0
        self.records = {}     # name -> [(field, ty, mutable)]
        self.variants = {}    # name -> [(ctor, [arg tys])]
        self.pvs = {}         # name -> [(tag, ty or None)], sub-of
        self.pv_sub = {}      # small pv name -> bigger pv name
        self.mods = []        # names of modules of signature S
        self.classes = []     # class names
        self.exns = []        # (name, [arg tys])
        self.gadt = None      # dict of gadt template names
        self.fcm = False
        self.nodes = {}       # node id -> closed constant
        self.ill = ill
        self.ill_at = None
        self.ill_done = False
        self.gen_count = 0
        self.mult = 1         # loop multiplier while generating a body
        self.cost = 0         # accumulated call cost of the current body
        self.budget = 50000

    # ---------- utilities ----------
    def fresh(self, p='x'):
        self.n += 1
        return '%s%d' % (p, self.n)

    def chance(self, p):
        return self.r.random() < p

    def pick(self, xs):
        return self.r.choice(xs)

    def tstr(self, t):
        k = t[0]
        if k in ('int', 'bool', 'string', 'float', 'char', 'unit'):
            return k
        if k in ('list', 'option', 'ref', 'array', 'lazy', 'box'):
            return '(%s) %s' % (self.tstr(t[1]), k if k != 'lazy' else 'Lazy.t')
        if k == 'tuple':
            return '(' + ' * '.join(self.tstr(x) for x in t[1]) + ')'
        if k == 'arrow':
            lbl = t[1]
            a = self.tstr(t[2])
            pre = ''
            if lbl.startswith('~'):
                pre = lbl[1:] + ':'
            elif lbl.startswith('?'):
                pre = '?' + lbl[1:] + ':'
            return '(%s%s -> %s)' % (pre, a, self.tstr(t[3]))
        if k in ('rec', 'var', 'pv'):
            return t[1]
        if k == 'abs':
            return t[1] + '.t'
        if k == 'obj':
            return t[1]
        if k == 'g':
            return '(%s) %s' % (self.tstr(t[1]), self.gadt['ty'])
        if k == 'fcm':
            return '(module S)'
        raise ValueError(t)

    def comparable(self, t, seen=None):
        if seen is None:
            seen = set()
        k = t[0]
        if k in ('rec', 'var', 'pv'):
            if t in seen:
                return True
            seen = seen | {t}
        if k in ('arrow', 'obj', 'lazy', 'fcm', 'abs'):
            return False
        if k in ('list', 'option', 'ref', 'array', 'box', 'g'):
            return self.comparable(t[1], seen)
        if k == 'tuple':
            return all(self.comparable(x, seen) for x in t[1])
        if k == 'rec':
            return all(self.comparable(ft, seen) for _, ft, _ in self.records[t[1]])
        if k == 'var':
            return all(self.comparable(a, seen) for _, args in self.variants[t[1]]
                       for a in args)
        if k == 'pv':
            return all(p is None or self.comparable(p, seen)
                       for _, p in self.pvs[t[1]])
        return True

    def mark(self, e, t):
        """Wrap e as a minimizer node whose replacement is const(t)."""
        c = self.const(t)
        if c is None:
            return e
        self.n += 1
        nid = self.n
        self.nodes[nid] = c
        e.code = '%s%d%s%s%s' % (MARK_OPEN, nid, MARK_MID, e.code, MARK_CLOSE)
        return e

    # ---------- types ----------
    def rand_type(self, d=2, allow_arrow=True):
        r = self.r.random()
        if d <= 0 or r < 0.35:
            return self.pick(BASE)
        opts = ['list', 'option', 'tuple', 'decl', 'decl', 'array', 'ref', 'box']
        if allow_arrow:
            opts += ['arrow']
        if self.mods:
            opts.append('abs')
        if self.classes:
            opts.append('obj')
        if self.gadt:
            opts.append('g')
        if self.chance(0.05):
            opts.append('lazy')
        k = self.pick(opts)
        if k in ('list', 'option', 'array', 'ref', 'box', 'lazy'):
            return (k, self.rand_type(d - 1, allow_arrow and k != 'array'))
        if k == 'tuple':
            return ('tuple', tuple(self.rand_type(d - 1, allow_arrow)
                                   for _ in range(self.pick([2, 2, 3]))))
        if k == 'arrow':
            return arrow(self.rand_type(d - 1, False), self.rand_type(d - 1, False))
        if k == 'abs':
            return ('abs', self.pick(self.mods))
        if k == 'obj':
            return ('obj', self.pick(self.classes))
        if k == 'g':
            return ('g', self.gadt_type(d - 1))
        decls = [('rec', n) for n in self.records] + \
            [('var', n) for n in self.variants] + [('pv', n) for n in self.pvs]
        if decls:
            return self.pick(decls)
        return self.pick(BASE)

    def gadt_type(self, d):
        if d <= 0 or self.chance(0.6):
            return self.pick([INT, BOOL, STRING])
        return ('tuple', (self.gadt_type(d - 1), self.gadt_type(d - 1)))

    # ---------- closed constants (minimizer replacements) ----------
    def const(self, t, d=0):
        k = t[0]
        if d > 6:
            return None
        if k == 'int':
            return '0'
        if k == 'bool':
            return 'false'
        if k == 'string':
            return '""'
        if k == 'float':
            return '0.'
        if k == 'char':
            return "'a'"
        if k == 'unit':
            return '()'
        if k == 'list':
            return '[]'
        if k == 'option':
            return 'None'
        if k == 'box':
            return 'Empty'
        if k in ('ref', 'array', 'lazy'):
            c = self.const(t[1], d + 1)
            if c is None:
                return None
            return {'ref': '(ref (%s))', 'array': '[|%s|]',
                    'lazy': '(lazy (%s))'}[k] % c
        if k == 'tuple':
            cs = [self.const(x, d + 1) for x in t[1]]
            if None in cs:
                return None
            return '(' + ', '.join(cs) + ')'
        if k == 'arrow':
            c = self.const(t[3], d + 1)
            if c is None:
                return None
            lbl = t[1]
            if lbl.startswith('~'):
                return '(fun ~%s:_ -> %s)' % (lbl[1:], c)
            if lbl.startswith('?'):
                return '(fun ?%s:_ -> %s)' % (lbl[1:], c)
            return '(fun _ -> %s)' % c
        if k == 'rec':
            fs = []
            for f, ft, _ in self.records[t[1]]:
                c = self.const(ft, d + 1)
                if c is None:
                    return None
                fs.append('%s = %s' % (f, c))
            return '({%s} : %s)' % ('; '.join(fs), t[1])
        if k == 'var':
            for c, args in self.variants[t[1]]:
                if not args:
                    return '(%s : %s)' % (c, t[1])
            c, args = self.variants[t[1]][0]
            cs = [self.const(a, d + 1) for a in args]
            if None in cs:
                return None
            return '(%s (%s) : %s)' % (c, ', '.join(cs), t[1])
        if k == 'pv':
            for tag, p in self.pvs[t[1]]:
                if p is None:
                    return '(`%s : %s)' % (tag, t[1])
            tag, p = self.pvs[t[1]][0]
            c = self.const(p, d + 1)
            return None if c is None else '(`%s (%s) : %s)' % (tag, c, t[1])
        if k == 'abs':
            return '(%s.make 0)' % t[1]
        if k == 'obj':
            return '(new %s 0)' % t[1]
        if k == 'g':
            return self.gadt_const(t[1])
        if k == 'fcm':
            return None
        return None

    def gadt_const(self, t):
        g = self.gadt
        if t == INT:
            return '(%s 0)' % g['int']
        if t == BOOL:
            return '(%s false)' % g['bool']
        if t == STRING:
            return '(%s "")' % g['str']
        if t[0] == 'tuple':
            a = self.gadt_const(t[1][0])
            b = self.gadt_const(t[1][1])
            return '(%s (%s, %s))' % (g['pair'], a, b)
        return None

    # ---------- literals ----------
    def int_pat(self):
        while True:
            l = self.int_lit()
            if not l[0].isalpha():
                return l.replace('(-0x10)', '(-16)')

    def int_lit(self):
        r = self.r.random()
        if r < 0.5:
            v = self.r.randint(-3, 12)
        elif r < 0.8:
            v = self.r.randint(-1000, 100000)
        elif r < 0.9:
            return self.pick(['max_int', 'min_int', '0x7fff', '0o17', '0b101',
                              '1_000', '(-0x10)'])
        else:
            v = self.r.randint(-(1 << 62), (1 << 62) - 1)
        return str(v) if v >= 0 else '(%d)' % v

    def float_lit(self):
        r = self.r.random()
        if r < 0.1:
            return self.pick(['nan', 'infinity', 'neg_infinity', '(-0.)',
                              'epsilon_float', 'max_float', 'min_float',
                              '0x1.8p3', '1e300', '1e-300'])
        v = round(self.r.uniform(-1000, 1000), self.r.randint(0, 4))
        s = repr(v)
        if 'e' not in s and '.' not in s:
            s += '.'
        return s if v >= 0 and not s.startswith('-') else '(%s)' % s

    def esc_char(self, c):
        o = ord(c)
        if c == "'":
            return "\\'"
        if c == '\\':
            return '\\\\'
        if c == '"':
            return '"'
        if c == '\n':
            return '\\n'
        if c == '\t':
            return '\\t'
        if 32 <= o < 127:
            return c
        return '\\%03d' % o

    def rand_char(self):
        r = self.r.random()
        if r < 0.7:
            return chr(self.r.randint(97, 122))
        if r < 0.9:
            return self.pick(list(' 09AZ_\'"\\\n\t.,;'))
        return chr(self.r.randint(0, 255))

    def char_lit(self):
        return "'%s'" % self.esc_char(self.rand_char())

    def string_lit(self):
        n = self.pick([0, 1, 2, 3, 5, 8])
        s = ''.join(self.rand_char() for _ in range(n))
        out = []
        for c in s:
            if c == '"':
                out.append('\\"')
            elif c == "'":
                out.append("'")
            else:
                out.append(self.esc_char(c))
        if self.chance(0.05):
            return '{|%s|}' % ''.join(c for c in s if c.isalnum() or c == ' ')
        return '"%s"' % ''.join(out)

    # ---------- show ----------
    def show(self, t):
        k = t[0]
        simple = {'int': 'string_of_int', 'bool': 'string_of_bool',
                  'string': 'show_string', 'float': 'show_float',
                  'char': 'show_char', 'unit': 'show_unit'}
        if k in simple:
            return simple[k]
        if k in ('list', 'option', 'array', 'ref', 'lazy', 'box'):
            return '(show_%s %s)' % (k, self.show(t[1]))
        if k == 'tuple':
            vs = [self.fresh('s') for _ in t[1]]
            parts = ' ^ "," ^ '.join('%s %s' % (self.show(x), v)
                                     for x, v in zip(t[1], vs))
            return '(fun (%s) -> "(" ^ %s ^ ")")' % (', '.join(vs), parts)
        if k == 'arrow':
            return 'show_fun'
        if k in ('rec', 'var', 'pv'):
            return 'show_' + t[1]
        if k == 'abs':
            return t[1] + '.show'
        if k == 'obj':
            return '(fun o -> "obj" ^ string_of_int o#get)'
        if k == 'g':
            return '(fun g -> "g:" ^ %s (%s g))' % (self.show(t[1]),
                                                   self.gadt['eval'])
        if k == 'fcm':
            return '(fun (module X : S) -> X.show (X.make 1))'
        raise ValueError(t)

    # ---------- expression generation ----------
    def combine(self, parts, build):
        """parts: list of E, evaluated in unspecified order by `build`.  If
        more than one has effects, let-bind the effectful ones left to
        right."""
        effs = [p for p in parts if p.eff]
        if len(effs) <= 1:
            return E(build([p.code for p in parts]),
                     eff=bool(effs))
        pre = []
        codes = []
        for p in parts:
            if p.eff:
                v = self.fresh('h')
                pre.append('let %s = %s in ' % (v, p.code))
                codes.append(v)
            else:
                codes.append(p.code)
        return E('(' + ''.join(pre) + build(codes) + ')', eff=True)

    def gen(self, t, env, d):
        self.gen_count += 1
        if self.ill and not self.ill_done and self.gen_count == self.ill_at:
            self.ill_done = True
            return self.ill_expr(t, env)
        e = self.gen1(t, env, d)
        if self.chance(0.06) and enclosed(e.code):
            # inside the expression's own parentheses, where the attribute
            # attaches by precedence (`(a + b [@fz])`: to the sum)
            e = E('%s [@fz%s])' % (e.code[:-1], self.pick(['', ' 1', ' "a"'])), e.eff, e.ceff)
        return self.mark(e, t)

    SYNTAX_ERRORS = [
        'Some 1 2', '(- 1 2 3 [@a] 4)', '(1 :: [] [@a] [] :: [])', '(f x [@a] y)', '(a #+)',
        '(let x = in x)', '((1)', '(begin 1)', '(1;;)', '[1;;2]', '(let x = 1 x)',
        '(match x with | -> 1)', '(fun -> 1)', '(if then 1 else 2)', '{a = }', '(1 : )',
        '(x.)', '(x.())', '[|1;', '(1 +)', '(:: 1)', '(`)', '(1 [@)', '(x <- )',
        '(try 1 with)', '(function)', '(lazy)', '(assert)', '(new)', '(object end 1)',
        '(Some (1) 2)', '(`A 1 2)', '(true 1)', '(-.1 2.)', '(f ~x)', '(f ~x: 1)',
        '(let open in 1)', '(M.)', '(x : int :> )', '(x :> int : int)', '(1 ;; 2)',
    ]

    def ill_expr(self, t, env):
        if self.chance(0.3):
            return E(self.pick(self.SYNTAX_ERRORS))
        r = self.r.random()
        if r < 0.15:
            return E('unbound_' + self.pick(['x', 'f', 'foo']))
        if r < 0.25 and t != STRING:
            return E('"ill"')
        if r < 0.35 and t != INT:
            return E('42')
        if r < 0.45:
            return E('(fun x -> x) 1 2')
        if r < 0.55 and t[0] == 'list':
            return E('[1; "a"]')
        if r < 0.65:
            return E('(%s : %s)' % (self.const(INT) if t != INT else '"s"',
                                    self.tstr(t)))
        if r < 0.75 and self.records:
            n = self.pick(list(self.records))
            return E('{%s with nofield = 1}' % self.const(('rec', n)))
        other = self.rand_type(2)
        while other == t:
            other = self.rand_type(2)
        return self.gen1(other, env, 1)

    def vars_of(self, env, t):
        return [v for v in env if isinstance(v, Var) and v.ty == t]

    def callables(self, env, t):
        """Vars whose type ends in t after positional/labeled params."""
        out = []
        for v in env:
            if not isinstance(v, Var) or v.ty[0] != 'arrow':
                continue
            params = []
            ty = v.ty
            while ty[0] == 'arrow':
                params.append((ty[1], ty[2]))
                ty = ty[3]
                if ty == t and not params[-1][0].startswith('?'):
                    out.append((v, list(params)))
        return out

    def add_cost(self, c):
        self.cost += c * self.mult

    def affordable(self, v):
        return self.cost + v.cost * self.mult <= self.budget

    def gen_call(self, v, params, env, d):
        """Full application of v to `params` (labels may be permuted,
        optionals omitted)."""
        self.add_cost(v.cost)
        args = []
        for idx, (lbl, pt) in enumerate(params):
            if lbl.startswith('?'):
                r = self.r.random()
                if r < 0.35 and any(l == '' for l, _ in params[idx + 1:]):
                    continue
                a = self.gen(pt, env, d - 1)
                if r < 0.75:
                    args.append((lbl, '~%s:' % lbl[1:], a))
                else:
                    args.append((lbl, '?%s:' % lbl[1:],
                                 E('(Some %s)' % a.code, a.eff)))
            elif lbl.startswith('~'):
                args.append((lbl, '~%s:' % lbl[1:], self.gen(pt, env, d - 1)))
            else:
                args.append((lbl, '', self.gen(pt, env, d - 1)))
        # labeled arguments may commute; positional keep their order
        if any(a[0] for a in args) and self.chance(0.4):
            lab = [a for a in args if a[0]]
            self.r.shuffle(lab)
            it = iter(lab)
            args = [next(it) if a[0] else a for a in args]
        exprs = [a[2] for a in args]
        prefixes = [a[1] for a in args]

        def build(cs):
            return '(%s %s)' % (v.name, ' '.join(
                '%s(%s)' % (p, c) if p else '(%s)' % c
                for p, c in zip(prefixes, cs)))
        e = self.combine(exprs, build)
        if v.ceff:
            e.eff = True
        return e

    def gen_poly(self, t, env, d):
        polys = [p for p in env if isinstance(p, Poly)]
        self.r.shuffle(polys)
        for p in polys:
            params = []
            ty = p.ty
            while ty[0] == 'arrow':
                params.append(ty[2])
                ty = ty[3]
            # try matching result after k params, k = all params
            s = {}
            if not match_ty(ty, t, s):
                continue
            free = set()
            for pt in params:
                tvs(pt, free)
            for f in free:
                if f not in s:
                    s[f] = self.rand_type(1, False)
            args = [self.gen(subst(pt, s), env, d - 1) for pt in params]
            e = self.combine(args, lambda cs: '(%s %s)' % (
                p.name, ' '.join('(%s)' % c for c in cs)))
            if p.calls_args and any(a.ceff for a in args):
                e.eff = True
            if p.name == 'Lazy.force':
                e.eff = True
            if any(subst(pt, s)[0] == 'arrow' for pt in params) and \
                    any(a.ceff for a in args):
                e.ceff = True
            if p.name.startswith('List.') or p.name.startswith('Array.'):
                self.add_cost(3)
            return e
        return None

    def leaf(self, t, env):
        vs = self.vars_of(env, t)
        if vs and self.chance(0.6):
            v = self.pick(vs)
            return E(v.name, ceff=v.ceff)
        return self.gen_lit(t, env)

    def gen_lit(self, t, env):
        k = t[0]
        if k == 'int':
            return E(self.int_lit())
        if k == 'bool':
            return E(self.pick(['true', 'false']))
        if k == 'string':
            return E(self.string_lit())
        if k == 'float':
            return E(self.float_lit())
        if k == 'char':
            return E(self.char_lit())
        if k == 'unit':
            return E('()')
        c = self.const(t)
        if c is not None and t[0] not in ('arrow', 'tuple', 'rec', 'var', 'pv'):
            return E(c)
        return self.gen_type(t, env, 0)

    def gen1(self, t, env, d):
        if d <= 0:
            return self.leaf(t, env)
        r = self.r.random()
        # generic forms, any type
        if r < 0.12:
            vs = self.vars_of(env, t)
            if vs:
                v = self.pick(vs)
                return E(v.name, ceff=v.ceff)
        elif r < 0.24:
            cs = [c for c in self.callables(env, t) if self.affordable(c[0])]
            if cs:
                v, ps = self.pick(cs)
                return self.gen_call(v, ps, env, d)
        elif r < 0.30:
            e = self.gen_poly(t, env, d)
            if e:
                return e
        elif r < 0.37:
            return self.gen_let(t, env, d)
        elif r < 0.43:
            c = self.gen(BOOL, env, d - 1)
            a = self.gen(t, env, d - 1)
            b = self.gen(t, env, d - 1)
            return E('(if %s then %s else %s)' % (c.code, a.code, b.code),
                     c.eff or a.eff or b.eff, a.ceff or b.ceff)
        elif r < 0.50:
            return self.gen_match(t, env, d)
        elif r < 0.53 and self.exns:
            return self.gen_try(t, env, d)
        elif r < 0.55 and self.exns:
            c = self.gen(BOOL, env, d - 1)
            ex, args = self.pick(self.exns)
            raised = self.raise_code(ex, args, env, d)
            b = self.gen(t, env, d - 1)
            return E('(if %s then %s else %s)' % (c.code, raised, b.code),
                     True, b.ceff)
        elif r < 0.57:
            s = self.gen_stmt(env, d - 1)
            b = self.gen(t, env, d - 1)
            return E('(%s; %s)' % (s, b.code), True, b.ceff)
        elif r < 0.59 and self.mods:
            m = self.pick(self.mods)
            b = self.gen(t, env, d - 1)
            if self.chance(0.5):
                return E('(let open %s in %s)' % (m, b.code), b.eff, b.ceff)
            return E('%s.(%s)' % (m, b.code), b.eff, b.ceff)
        elif r < 0.61:
            b = self.gen(t, env, d - 1)
            return E('(%s : %s)' % (b.code, self.tstr(t)), b.eff, b.ceff)
        elif r < 0.62:
            ex = self.fresh('LE')
            b = self.gen(t, env, d - 1)
            alt = self.gen(t, env, d - 1)
            c = self.gen(BOOL, env, d - 1)
            return E('(let exception %s in try (if %s then raise %s else %s) '
                     'with %s -> %s)' % (ex, c.code, ex, b.code, ex, alt.code),
                     True, b.ceff or alt.ceff)
        elif r < 0.63:
            name = self.fresh('LM')
            v = self.fresh('lv')
            a = self.gen(INT, env, d - 1)
            env2 = env + [Var('%s.%s' % (name, v), INT)]
            b = self.gen(t, env2, d - 1)
            return E('(let module %s = struct let %s = %s end in %s)' %
                     (name, v, a.code, b.code), a.eff or b.eff, b.ceff)
        return self.gen_type(t, env, d)

    def raise_code(self, ex, args, env, d):
        if not args:
            return '(raise %s)' % ex
        es = [self.gen(a, env, d - 1) for a in args]
        e = self.combine(es, lambda cs: '(raise (%s (%s)))' % (ex, ', '.join(cs)))
        return e.code

    def gen_let(self, t, env, d):
        r = self.r.random()
        if r < 0.15:
            # let-polymorphism: a local polymorphic function used at several types
            f = self.fresh('pf')
            tmpl = self.pick([
                ('fun x -> (x, %s)' % self.int_lit(),
                 arrow(TV[0], ('tuple', (TV[0], INT)))),
                ('fun x -> [x; x]', arrow(TV[0], ('list', TV[0]))),
                ('fun x -> Some x', arrow(TV[0], ('option', TV[0]))),
                ('fun f x -> f (f x)', arrow(arrow(TV[0], TV[0]),
                                             arrow(TV[0], TV[0]))),
                ('fun x y -> if %s then x else y' % self.pick(['true', 'false']),
                 arrow(TV[0], arrow(TV[0], TV[0]))),
                ('fun (x, _) -> x', arrow(('tuple', (TV[0], TV[1])), TV[0])),
            ])
            b = self.gen(t, env + [Poly(f, tmpl[1], 'f f' in tmpl[0])], d - 1)
            return E('(let %s = %s in %s)' % (f, tmpl[0], b.code), b.eff, b.ceff)
        if r < 0.25:
            # local function
            f = self.fresh('lf')
            pt = self.rand_type(1, False)
            rt = self.rand_type(1, False)
            p = self.fresh('p')
            save = self.cost
            self.cost = 0
            body = self.gen(rt, env + [Var(p, pt)], d - 1)
            cost = self.cost + 1
            self.cost = save
            b = self.gen(t, env + [Var(f, arrow(pt, rt), body.eff, cost)], d - 1)
            ann = ''
            if self.chance(0.3):
                ann = '[@inline never]' if self.chance(0.5) else '[@inline]'
            return E('(let%s %s (%s : %s) = %s in %s)' % (
                ann, f, p, self.tstr(pt), body.code, b.code), b.eff, b.ceff)
        if r < 0.32:
            # tuple destructuring
            ts = (self.rand_type(1), self.rand_type(1))
            a = self.gen(('tuple', ts), env, d - 1)
            x, y = self.fresh(), self.fresh()
            b = self.gen(t, env + [Var(x, ts[0]), Var(y, ts[1])], d - 1)
            return E('(let (%s, %s) = %s in %s)' % (x, y, a.code, b.code),
                     a.eff or b.eff, b.ceff)
        if r < 0.38:
            # lazy
            lt = self.rand_type(1, False)
            a = self.gen(lt, env, d - 1)
            x = self.fresh('lz')
            b = self.gen(t, env + [Var(x, ('lazy', lt))], d - 1)
            return E('(let %s = lazy (%s) in %s)' % (x, a.code, b.code),
                     b.eff, b.ceff)
        st = self.rand_type(2)
        a = self.gen(st, env, d - 1)
        x = self.fresh()
        b = self.gen(t, env + [Var(x, st, a.ceff)], d - 1)
        ann = ''
        if self.chance(0.2):
            ann = ' : ' + self.tstr(st)
        return E('(let %s%s = %s in %s)' % (x, ann, a.code, b.code),
                 a.eff or b.eff, b.ceff)

    def gen_try(self, t, env, d):
        b = self.gen(t, env, d - 1)
        cases = []
        exns = self.exns[:]
        self.r.shuffle(exns)
        for ex, args in exns[:self.r.randint(1, len(exns))]:
            vs = [self.fresh('ea') for _ in args]
            env2 = env + [Var(v, a) for v, a in zip(vs, args)]
            h = self.gen(t, env2, d - 1)
            pat = ex if not args else '%s (%s)' % (ex, ', '.join(vs))
            cases.append('%s -> %s' % (pat, h.code))
        if self.chance(0.3):
            h = self.gen(t, env, d - 1)
            cases.append('Not_found | Invalid_argument _ -> %s' % h.code)
        return E('(try %s with %s)' % (b.code, ' | '.join(cases)), True, b.ceff)

    def gen_match(self, t, env, d):
        st = self.rand_type(2, False)
        if self.chance(0.4):
            cands = [v.ty for v in env if isinstance(v, Var) and
                     v.ty[0] != 'arrow']
            if cands:
                st = self.pick(cands)
        s = self.gen(st, env, d - 1)
        cases = []
        for _ in range(self.r.randint(1, 4)):
            p, binds = self.gen_pat(st, 2)
            env2 = env + [Var(n, ty) for n, ty in binds]
            guard = ''
            if self.chance(0.2):
                g = self.gen(BOOL, env2, 1)
                if not g.eff:
                    guard = ' when ' + g.code
            body = self.gen(t, env2, d - 1)
            cases.append('%s%s -> %s' % (p, guard, body.code))
        if self.exns and self.chance(0.15):
            ex, args = self.pick(self.exns)
            vs = [self.fresh('ea') for _ in args]
            body = self.gen(t, env + [Var(v, a) for v, a in zip(vs, args)], d - 1)
            pat = ex if not args else '%s (%s)' % (ex, ', '.join(vs))
            cases.append('exception %s -> %s' % (pat, body.code))
        dflt = self.gen(t, env, d - 1)
        cases.append('_ -> %s' % dflt.code)
        return E('(match (%s : %s) with %s)' % (s.code, self.tstr(st),
                                                ' | '.join(cases)), True, True)

    # ---------- patterns ----------
    def gen_pat(self, t, d):
        k = t[0]
        r = self.r.random()
        if d <= 0 or r < 0.15:
            if self.chance(0.5):
                return '_', []
            v = self.fresh('v')
            return v, [(v, t)]
        if r < 0.22:
            p, b = self.gen_pat(t, d - 1)
            v = self.fresh('al')
            return '(%s as %s)' % (p, v), b + [(v, t)]
        if k == 'int':
            if self.chance(0.3):
                return '(%s | %s)' % (self.int_pat(), self.int_pat()), []
            return self.int_pat(), []
        if k == 'char':
            if self.chance(0.4):
                a, b = sorted([self.r.randint(32, 126), self.r.randint(32, 126)])
                return "('%s'..'%s')" % (self.esc_char(chr(a)),
                                         self.esc_char(chr(b))), []
            return self.char_lit(), []
        if k == 'string':
            return self.string_lit(), []
        if k == 'bool':
            return self.pick(['true', 'false']), []
        if k == 'float':
            return self.float_lit() if self.chance(0.3) else '_', []
        if k == 'list':
            c = self.r.random()
            if c < 0.3:
                return '[]', []
            if c < 0.7:
                p1, b1 = self.gen_pat(t[1], d - 1)
                p2, b2 = self.gen_pat(t, d - 1)
                return '(%s :: %s)' % (p1, p2), b1 + b2
            ps = [self.gen_pat(t[1], d - 1) for _ in range(self.r.randint(1, 3))]
            return '[%s]' % '; '.join(p for p, _ in ps), \
                [x for _, b in ps for x in b]
        if k == 'option':
            if self.chance(0.3):
                return 'None', []
            p, b = self.gen_pat(t[1], d - 1)
            return '(Some %s)' % p, b
        if k == 'box':
            c = self.r.random()
            if c < 0.2:
                return 'Empty', []
            if c < 0.6:
                p, b = self.gen_pat(t[1], d - 1)
                return '(Box %s)' % p, b
            p1, b1 = self.gen_pat(t[1], d - 1)
            p2, b2 = self.gen_pat(t[1], d - 1)
            return '(Pair (%s, %s))' % (p1, p2), b1 + b2
        if k == 'tuple':
            ps = [self.gen_pat(x, d - 1) for x in t[1]]
            return '(%s)' % ', '.join(p for p, _ in ps), \
                [x for _, b in ps for x in b]
        if k == 'ref':
            p, b = self.gen_pat(t[1], d - 1)
            return '{contents = %s}' % p, b
        if k == 'array':
            ps = [self.gen_pat(t[1], d - 1) for _ in range(self.r.randint(1, 2))]
            return '[|%s|]' % '; '.join(p for p, _ in ps), \
                [x for _, b in ps for x in b]
        if k == 'rec':
            fs = self.records[t[1]]
            chosen = [f for f in fs if self.chance(0.6)] or [fs[0]]
            ps = [(f, self.gen_pat(ft, d - 1)) for f, ft, _ in chosen]
            rest = '; _' if len(chosen) < len(fs) else ''
            return '{%s%s}' % ('; '.join('%s = %s' % (f, p) for f, (p, _) in ps),
                               rest), [x for _, (_, b) in ps for x in b]
        if k == 'var':
            cs = self.variants[t[1]]
            if self.chance(0.25):
                consts = [c for c, a in cs if not a]
                if len(consts) >= 2:
                    return '(%s)' % ' | '.join(self.r.sample(consts, 2)), []
                # or-pattern binding the same variables
                same = {}
                for c, a in cs:
                    if len(a) == 1:
                        same.setdefault(a[0], []).append(c)
                for ty, ctors in same.items():
                    if len(ctors) >= 2:
                        v = self.fresh('o')
                        c1, c2 = self.r.sample(ctors, 2)
                        return '(%s %s | %s %s)' % (c1, v, c2, v), [(v, ty)]
            c, args = self.pick(cs)
            if not args:
                return c, []
            if self.chance(0.15):
                return '%s _' % c, []
            ps = [self.gen_pat(a, d - 1) for a in args]
            return '%s (%s)' % (c, ', '.join(p for p, _ in ps)), \
                [x for _, b in ps for x in b]
        if k == 'pv':
            for small, big in self.pv_sub.items():
                if big == t[1] and self.chance(0.3):
                    v = self.fresh('pv')
                    return '(#%s as %s)' % (small, v), [(v, ('pv', small))]
            tag, p = self.pick(self.pvs[t[1]])
            if p is None:
                return '`' + tag, []
            pp, b = self.gen_pat(p, d - 1)
            return '(`%s %s)' % (tag, pp), b
        v = self.fresh('v')
        return v, [(v, t)]

    # ---------- per-type constructions ----------
    def gen_type(self, t, env, d):
        k = t[0]
        g = getattr(self, 'ty_' + k)
        return g(t, env, d)

    def binop(self, t, ops, env, d, wrap=None):
        op = self.pick(ops)
        a = self.gen(t, env, d - 1)
        b = self.gen(t, env, d - 1)
        if wrap:
            return self.combine([a, b], lambda cs: wrap % (cs[0], op, cs[1]))
        return self.combine([a, b], lambda cs: '(%s %s %s)' % (cs[0], op, cs[1]))

    def ty_int(self, t, env, d):
        r = self.r.random()
        if r < 0.3:
            return self.binop(INT, ['+', '-', '*', 'land', 'lor', 'lxor'], env, d)
        if r < 0.38:
            a = self.gen(INT, env, d - 1)
            b = self.gen(INT, env, d - 1)
            op = self.pick(['/', 'mod'])
            return self.combine([a, b], lambda cs: '(%s %s (let y = %s in if y = 0 then 7 else y))' % (cs[0], op, cs[1]))
        if r < 0.43:
            a = self.gen(INT, env, d - 1)
            b = self.gen(INT, env, d - 1)
            op = self.pick(['lsl', 'lsr', 'asr'])
            return self.combine([a, b], lambda cs: '(%s %s ((%s) land 63))' % (cs[0], op, cs[1]))
        if r < 0.44:
            c = self.r.random()
            if c < 0.3:
                return E('(- %s)' % self.int_lit().strip('()'))
            if c < 0.5:
                a = self.gen(INT, env, d - 1)
                return E('(- %s)' % a.code, a.eff)
            if c < 0.7:
                fs = [v for v in env if isinstance(v, Var) and v.ty == arrow(INT, INT)
                      and self.affordable(v)]
                atoms = [self.int_lit()] + [v.name for v in env if isinstance(v, Var) and v.ty == INT]
                if fs:
                    f = self.pick(fs)
                    self.add_cost(f.cost)
                    return E('(%s %s #+ %s)' % (f.name, self.pick(atoms), self.pick(atoms)), f.ceff)
            return E('(%s #+ %s #+ %s)' % (self.pick([self.int_lit(), '1']), self.int_lit(),
                                           self.int_lit()))
        if r < 0.48:
            f = self.pick(['abs', '(~-)', 'succ', 'pred', 'lnot',
                           'Int.abs', 'Int.neg'])
            a = self.gen(INT, env, d - 1)
            return E('(%s %s)' % (f, a.code), a.eff)
        if r < 0.53:
            a = self.gen(STRING, env, d - 1)
            return E('(String.length %s)' % a.code, a.eff)
        if r < 0.56:
            a = self.gen(CHAR, env, d - 1)
            return E('(Char.code %s)' % a.code, a.eff)
        if r < 0.62:
            ct = self.rand_type(1, False)
            while not self.comparable(ct):
                ct = self.rand_type(1, False)
            return self.combine([self.gen(ct, env, d - 1), self.gen(ct, env, d - 1)],
                                lambda cs: '(compare %s %s)' % tuple(cs))
        if r < 0.65:
            a = self.gen(FLOAT, env, d - 1)
            return E('(let f = %s in if Float.is_finite f && Float.abs f < 1e15 '
                     'then int_of_float f else 0)' % a.code, a.eff)
        if r < 0.68:
            ct = self.rand_type(1, False)
            while not self.comparable(ct):
                ct = self.rand_type(1, False)
            a = self.gen(ct, env, d - 1)
            return E('(Hashtbl.hash %s)' % a.code, a.eff)
        if r < 0.71:
            return E(self.pick(['!gr', 'ga.(%d)' % self.r.randint(0, 7),
                                '(Hashtbl.length gh)', '(Buffer.length gb)',
                                '(String.length !gs)']), True)
        if r < 0.74 and self.mods:
            m = self.pick(self.mods)
            a = self.gen(('abs', m), env, d - 1)
            return E('(%s.get %s)' % (m, a.code), a.eff)
        if r < 0.77 and self.classes:
            o = self.gen(('obj', self.pick(self.classes)), env, d - 1)
            if self.chance(0.5):
                return E('(%s)#get' % o.code, True)
            a = self.gen(INT, env, d - 1)
            e = self.combine([o, a], lambda cs: '((%s)#bump %s)' % tuple(cs))
            e.eff = True
            return e
        if r < 0.80 and self.fcm:
            a = self.gen(INT, env, d - 1)
            return E('(List.fold_left (fun acc (module X : S) -> acc + '
                     'X.get (X.combine (X.make %s) (X.make acc))) 0 mods)' % a.code,
                     True)
        if r < 0.83 and self.gadt:
            gt = self.gadt_type(1)
            if gt == INT:
                a = self.gen(('g', INT), env, d - 1)
                return E('(%s %s)' % (self.gadt['eval'], a.code), a.eff)
            a = self.gen(('g', gt), env, d - 1)
            return E('(%s %s)' % (self.gadt['size'], a.code), a.eff)
        if r < 0.86:
            lt = self.rand_type(1, False)
            a = self.gen(('list', lt), env, d - 1)
            f = self.gen(arrow(INT, arrow(lt, INT)), env, d - 1)
            e = self.combine([f, a], lambda cs: '(List.fold_left %s 0 %s)' % tuple(cs))
            if f.ceff:
                e.eff = True
            return e
        if r < 0.88:
            a = self.gen(INT, env, d - 1)
            v = self.fresh('i')
            acc = self.fresh('acc')
            save = self.mult
            self.mult *= 10
            try:
                body = self.gen(INT, env + [Var(v, INT), Var(acc, INT)], d - 1)
            finally:
                self.mult = save
            return E('(let rec loop %s %s = if %s <= 0 then %s else loop (%s - 1) (%s) in '
                     'loop (min 10 (abs %s)) 0)' % (v, acc, v, acc, v, body.code, a.code),
                     a.eff or body.eff)
        return E(self.int_lit())

    def ty_bool(self, t, env, d):
        r = self.r.random()
        if r < 0.35:
            ct = self.rand_type(1, False)
            while not self.comparable(ct):
                ct = self.rand_type(1, False)
            op = self.pick(['=', '<>', '<', '<=', '>', '>='])
            return self.combine([self.gen(ct, env, d - 1), self.gen(ct, env, d - 1)],
                                lambda cs: '(%s %s %s)' % (cs[0], op, cs[1]))
        if r < 0.5:
            op = self.pick(['&&', '||'])
            a = self.gen(BOOL, env, d - 1)
            b = self.gen(BOOL, env, d - 1)
            return E('(%s %s %s)' % (a.code, op, b.code), a.eff or b.eff)
        if r < 0.6:
            a = self.gen(BOOL, env, d - 1)
            return E('(not %s)' % a.code, a.eff)
        if r < 0.7:
            lt = self.rand_type(1, False)
            while not self.comparable(lt):
                lt = self.rand_type(1, False)
            return self.combine([self.gen(lt, env, d - 1),
                                 self.gen(('list', lt), env, d - 1)],
                                lambda cs: '(List.mem %s %s)' % tuple(cs))
        if r < 0.78:
            a = self.gen(FLOAT, env, d - 1)
            f = self.pick(['Float.is_nan', 'Float.is_integer', 'Float.is_finite',
                           'Float.sign_bit'])
            return E('(%s %s)' % (f, a.code), a.eff)
        if r < 0.85:
            return E('(Hashtbl.mem gh %s)' % self.gen(INT, env, d - 1).code, True)
        return E(self.pick(['true', 'false']))

    def ty_string(self, t, env, d):
        r = self.r.random()
        if r < 0.25:
            return self.binop(STRING, ['^'], env, d, '(cap (%s %s %s))')
        if r < 0.32:
            a = self.gen(INT, env, d - 1)
            return E('(string_of_int %s)' % a.code, a.eff)
        if r < 0.42:
            return self.gen_sprintf(env, d)
        if r < 0.48:
            a = self.gen(STRING, env, d - 1)
            i = self.gen(INT, env, d - 1)
            j = self.gen(INT, env, d - 1)
            e = self.combine([a, i, j], lambda cs: '(try String.sub %s %s %s with Invalid_argument m -> m)' % tuple(cs))
            e.eff = True
            return e
        if r < 0.53:
            f = self.pick(['String.uppercase_ascii', 'String.lowercase_ascii',
                           'String.escaped', 'String.trim', 'String.capitalize_ascii'])
            a = self.gen(STRING, env, d - 1)
            return E('(%s %s)' % (f, a.code), a.eff)
        if r < 0.58:
            a = self.gen(('list', STRING), env, d - 1)
            sep = self.gen(STRING, env, d - 1)
            return self.combine([sep, a], lambda cs: '(cap (String.concat %s %s))' % tuple(cs))
        if r < 0.62:
            n = self.gen(INT, env, d - 1)
            c = self.gen(CHAR, env, d - 1)
            return self.combine([n, c], lambda cs: '(String.make ((%s) land 7) %s)' % tuple(cs))
        if r < 0.66:
            a = self.gen(FLOAT, env, d - 1)
            return E('(string_of_float %s)' % a.code, a.eff)
        if r < 0.68:
            vs = [v.name for v in env if isinstance(v, Var) and v.ty == STRING]
            return E('(%s #^ %s)' % (self.pick(vs + [self.string_lit()]), self.string_lit()))
        if r < 0.70:
            a = self.gen(STRING, env, d - 1)
            v = self.fresh('c')
            body = self.gen(CHAR, env + [Var(v, CHAR)], 1)
            return E('(String.map (fun %s -> %s) %s)' % (v, body.code, a.code),
                     a.eff or body.eff)
        if r < 0.73:
            return E(self.pick(['!gs', '(Buffer.contents gb)',
                                '(Option.value ~default:"" (Hashtbl.find_opt gh 1))']), True)
        if r < 0.76:
            a = self.gen(INT, env, d - 1)
            return E('(try Hashtbl.find gh %s with Not_found -> "nf")' % a.code, True)
        return E(self.string_lit())

    def gen_sprintf(self, env, d):
        specs = []
        fmt = []
        for _ in range(self.r.randint(1, 4)):
            c = self.r.random()
            if c < 0.15:
                fmt.append(self.pick(['a', '-', ' ', ':', '%%', '@', 'x=']))
                continue
            spec, ty = self.pick([('%d', INT), ('%i', INT), ('%x', INT),
                                  ('%5d', INT), ('%-4d', INT), ('%X', INT),
                                  ('%o', INT), ('%s', STRING), ('%S', STRING),
                                  ('%-6s', STRING), ('%c', CHAR), ('%C', CHAR),
                                  ('%B', BOOL), ('%b', BOOL), ('%.3f', FLOAT),
                                  ('%g', FLOAT), ('%e', FLOAT), ('%h', FLOAT),
                                  ('%F', FLOAT), ('%08.2f', FLOAT), ('%+d', INT)])
            fmt.append(spec)
            specs.append(self.gen(ty, env, d - 1))
        return self.combine(specs, lambda cs: '(Printf.sprintf "%s"%s)' % (
            ''.join(fmt), ''.join(' (%s)' % c for c in cs)))

    def ty_float(self, t, env, d):
        r = self.r.random()
        if r < 0.35:
            return self.binop(FLOAT, ['+.', '-.', '*.', '/.', '**'], env, d)
        if r < 0.38:
            if self.chance(0.3):  # `-` folds into a float literal
                lit = self.float_lit().strip('()').lstrip('-')
                # `-`/`+` make a float only of a literal: `- nan` is int negation
                ops = ['-', '+', '-.'] if lit[:1].isdigit() else ['-.', '+.']
                return E('(%s %s)' % (self.pick(ops), lit))
            a = self.gen(FLOAT, env, d - 1)
            return E('(%s %s)' % (self.pick(['-.', '+.']), a.code), a.eff)
        if r < 0.45:
            f = self.pick(['sqrt', 'Float.abs', 'Float.neg', 'floor', 'ceil',
                           'Float.round', 'exp', 'log', 'sin', 'Float.trunc',
                           '(~-.)', 'Float.succ'])
            a = self.gen(FLOAT, env, d - 1)
            return E('(%s %s)' % (f, a.code), a.eff)
        if r < 0.55:
            a = self.gen(INT, env, d - 1)
            return E('(float_of_int %s)' % a.code, a.eff)
        if r < 0.6:
            f = self.pick(['Float.min', 'Float.max', 'Float.rem', 'Float.copy_sign',
                           'atan2', 'hypot'])
            return self.combine([self.gen(FLOAT, env, d - 1), self.gen(FLOAT, env, d - 1)],
                                lambda cs: '(%s %s %s)' % (f, cs[0], cs[1]))
        if r < 0.65:
            a = self.gen(STRING, env, d - 1)
            return E('(Option.value ~default:0.5 (float_of_string_opt %s))' % a.code, a.eff)
        return E(self.float_lit())

    def ty_char(self, t, env, d):
        r = self.r.random()
        if r < 0.3:
            a = self.gen(INT, env, d - 1)
            return E('(Char.chr ((%s) land 255))' % a.code, a.eff)
        if r < 0.45:
            a = self.gen(CHAR, env, d - 1)
            f = self.pick(['Char.uppercase_ascii', 'Char.lowercase_ascii'])
            return E('(%s %s)' % (f, a.code), a.eff)
        if r < 0.55:
            a = self.gen(STRING, env, d - 1)
            return E('(let s = %s in if s = "" then \'z\' else s.[String.length s - 1])' % a.code, a.eff)
        return E(self.char_lit())

    def ty_unit(self, t, env, d):
        return E('(%s)' % self.gen_stmt(env, d), True)

    def ty_list(self, t, env, d):
        r = self.r.random()
        et = t[1]
        if r < 0.15:
            return E('[]')
        if r < 0.4:
            a = self.gen(et, env, d - 1)
            b = self.gen(t, env, d - 1)
            return self.combine([a, b], lambda cs: '(%s :: %s)' % tuple(cs))
        if r < 0.55:
            es = [self.gen(et, env, d - 1) for _ in range(self.r.randint(1, 3))]
            return self.combine(es, lambda cs: '[%s]' % '; '.join(cs))
        if r < 0.62:
            return self.combine([self.gen(t, env, d - 1), self.gen(t, env, d - 1)],
                                lambda cs: '(%s @ %s)' % tuple(cs))
        if r < 0.72:
            n = self.gen(INT, env, d - 1)
            v = self.fresh('i')
            save = self.mult
            self.mult *= 6
            try:
                body = self.gen(et, env + [Var(v, INT)], d - 1)
            finally:
                self.mult = save
            return E('(List.init ((%s) land 3) (fun %s -> %s))' % (n.code, v, body.code),
                     n.eff or body.eff)
        if r < 0.8:
            src = self.rand_type(1, False)
            a = self.gen(('list', src), env, d - 1)
            f = self.gen(arrow(src, et), env, d - 1)
            e = self.combine([f, a], lambda cs: '(List.map %s %s)' % tuple(cs))
            if f.ceff:
                e.eff = True
            return e
        if r < 0.86:
            a = self.gen(t, env, d - 1)
            f = self.gen(arrow(et, BOOL), env, d - 1)
            e = self.combine([f, a], lambda cs: '(List.filter %s %s)' % tuple(cs))
            if f.ceff:
                e.eff = True
            return e
        if r < 0.9 and self.comparable(et):
            a = self.gen(t, env, d - 1)
            return E('(List.sort compare %s)' % a.code, a.eff)
        if r < 0.94:
            a = self.gen(('array', et), env, d - 1)
            return E('(Array.to_list %s)' % a.code, a.eff)
        return E('[]')

    def ty_option(self, t, env, d):
        r = self.r.random()
        if r < 0.25:
            return E('None')
        if r < 0.75:
            a = self.gen(t[1], env, d - 1)
            return E('(Some %s)' % a.code, a.eff)
        a = self.gen(('list', t[1]), env, d - 1)
        return E('(pfirst %s)' % a.code, a.eff)

    def ty_box(self, t, env, d):
        r = self.r.random()
        if r < 0.15:
            return E('Empty')
        if r < 0.6:
            a = self.gen(t[1], env, d - 1)
            return E('(Box %s)' % a.code, a.eff)
        return self.combine([self.gen(t[1], env, d - 1), self.gen(t[1], env, d - 1)],
                            lambda cs: '(Pair (%s, %s))' % tuple(cs))

    def ty_lazy(self, t, env, d):
        a = self.gen(t[1], env, d - 1)
        if self.chance(0.3):
            return E('(Lazy.from_val %s)' % a.code, a.eff)
        return E('(lazy %s)' % a.code)

    def ty_tuple(self, t, env, d):
        es = [self.gen(x, env, d - 1) for x in t[1]]
        return self.combine(es, lambda cs: '(%s)' % ', '.join(cs))

    def ty_ref(self, t, env, d):
        a = self.gen(t[1], env, d - 1)
        return E('(ref %s)' % a.code, True)

    def ty_array(self, t, env, d):
        r = self.r.random()
        if r < 0.4:
            es = [self.gen(t[1], env, d - 1) for _ in range(self.r.randint(1, 3))]
            e = self.combine(es, lambda cs: '[|%s|]' % '; '.join(cs))
            e.eff = True
            return e
        if r < 0.7:
            n = self.gen(INT, env, d - 1)
            a = self.gen(t[1], env, d - 1)
            e = self.combine([n, a], lambda cs: '(Array.make (1 + ((%s) land 3)) %s)' % tuple(cs))
            e.eff = True
            return e
        n = self.gen(INT, env, d - 1)
        v = self.fresh('i')
        body = self.gen(t[1], env + [Var(v, INT)], d - 1)
        return E('(Array.init (1 + ((%s) land 3)) (fun %s -> %s))' % (n.code, v, body.code), True)

    def ty_arrow(self, t, env, d):
        lbl, a, b = t[1], t[2], t[3]
        r = self.r.random()
        if r < 0.75 or lbl != '':
            p = self.fresh('p')
            save = self.mult
            self.mult *= 8
            try:
                body = self.gen(b, env + [Var(p, a)], d - 1)
            finally:
                self.mult = save
            if lbl.startswith('~'):
                head = '~%s:(%s : %s)' % (lbl[1:], p, self.tstr(a))
            elif lbl.startswith('?'):
                head = '?%s:(%s : %s = %s)' % (lbl[1:], p, self.tstr(a),
                                               self.const(a) or '0')
            else:
                head = p
            return E('(fun %s -> %s)' % (head, body.code), ceff=body.eff)
        # function by cases
        cases = []
        for _ in range(self.r.randint(1, 3)):
            pat, binds = self.gen_pat(a, 2)
            body = self.gen(b, env + [Var(n, ty) for n, ty in binds], d - 1)
            cases.append('%s -> %s' % (pat, body.code))
        dflt = self.gen(b, env, d - 1)
        cases.append('_ -> %s' % dflt.code)
        x = self.fresh('fx')
        return E('(fun (%s : %s) -> match %s with %s)' % (x, self.tstr(a), x, ' | '.join(cases)), ceff=True)

    def ty_rec(self, t, env, d):
        fs = self.records[t[1]]
        vs = self.vars_of(env, t)
        if vs and self.chance(0.3):
            base = self.pick(vs).name
            f, ft, _ = self.pick(fs)
            a = self.gen(ft, env, d - 1)
            return E('({%s with %s = %s} : %s)' % (base, f, a.code, t[1]), a.eff)
        es = [self.gen(ft, env, d - 1) for _, ft, _ in fs]
        e = self.combine(es, lambda cs: '({%s} : %s)' % (
            '; '.join('%s = %s' % (f, c) for (f, _, _), c in zip(fs, cs)), t[1]))
        if any(m for _, _, m in fs):
            e.eff = True
        return e

    def ty_var(self, t, env, d):
        c, args = self.pick(self.variants[t[1]])
        if not args:
            return E('(%s : %s)' % (c, t[1]))
        es = [self.gen(a, env, d - 1) for a in args]
        return self.combine(es, lambda cs: '(%s (%s) : %s)' % (c, ', '.join(cs), t[1]))

    def ty_pv(self, t, env, d):
        for small, big in self.pv_sub.items():
            if big == t[1] and self.chance(0.25):
                a = self.gen(('pv', small), env, d - 1)
                return E('(%s : %s :> %s)' % (a.code, small, big), a.eff)
        tag, p = self.pick(self.pvs[t[1]])
        if p is None:
            return E('(`%s : %s)' % (tag, t[1]))
        a = self.gen(p, env, d - 1)
        return E('(`%s %s : %s)' % (tag, a.code, t[1]), a.eff)

    def ty_abs(self, t, env, d):
        m = t[1]
        if self.chance(0.3):
            return self.combine([self.gen(t, env, d - 1), self.gen(t, env, d - 1)],
                                lambda cs: '(%s.combine %s %s)' % (m, cs[0], cs[1]))
        a = self.gen(INT, env, d - 1)
        return E('(%s.make %s)' % (m, a.code), a.eff)

    def ty_obj(self, t, env, d):
        c = t[1]
        a = self.gen(INT, env, d - 1)
        subs = [s for s, base in self.classes_sub if base == c]
        if subs and self.chance(0.4):
            return E('(new %s %s :> %s)' % (self.pick(subs), a.code, c), True)
        return E('(new %s %s)' % (c, a.code), True)

    def ty_g(self, t, env, d):
        g = self.gadt
        et = t[1]
        r = self.r.random()
        if d <= 1 or r < 0.3:
            if et == INT:
                return E('(%s %s)' % (g['int'], self.gen(INT, env, 0).code))
            if et == BOOL:
                return E('(%s %s)' % (g['bool'], self.gen(BOOL, env, 0).code))
            if et == STRING:
                return E('(%s %s)' % (g['str'], self.gen(STRING, env, 0).code))
        if r < 0.5:
            c = self.gen(('g', BOOL), env, d - 1)
            a = self.gen(t, env, d - 1)
            b = self.gen(t, env, d - 1)
            return self.combine([c, a, b], lambda cs: '(%s (%s, %s, %s))' % (
                g['if'], cs[0], cs[1], cs[2]))
        if et == INT and r < 0.7:
            return self.combine([self.gen(t, env, d - 1), self.gen(t, env, d - 1)],
                                lambda cs: '(%s (%s, %s))' % (g['add'], cs[0], cs[1]))
        if et == INT and r < 0.8:
            a = self.gen(('g', STRING), env, d - 1)
            return E('(%s %s)' % (g['len'], a.code), a.eff)
        if et == BOOL and r < 0.75:
            return self.combine([self.gen(('g', INT), env, d - 1),
                                 self.gen(('g', INT), env, d - 1)],
                                lambda cs: '(%s (%s, %s))' % (g['eq'], cs[0], cs[1]))
        if et[0] == 'tuple':
            return self.combine([self.gen(('g', et[1][0]), env, d - 1),
                                 self.gen(('g', et[1][1]), env, d - 1)],
                                lambda cs: '(%s (%s, %s))' % (g['pair'], cs[0], cs[1]))
        if r < 0.9:
            other = self.gadt_type(0)
            return E('(%s %s)' % (g['fst'], self.gen(('g', ('tuple', (et, other))), env, d - 1).code))
        return E(self.gadt_const(et))

    def ty_fcm(self, t, env, d):
        return E('(%s)' % self.pick(['(module %s : S)' % m for m in self.mods]))

    # ---------- statements ----------
    def gen_stmt(self, env, d):
        r = self.r.random()
        if r < 0.2:
            a = self.gen(INT, env, d - 1)
            return 'gr := %s' % a.code
        if r < 0.3:
            a = self.gen(STRING, env, d - 1)
            return 'gs := cap (!gs ^ %s)' % a.code
        if r < 0.4:
            i = self.gen(INT, env, d - 1)
            a = self.gen(INT, env, d - 1)
            e = self.combine([i, a], lambda cs: 'ga.((%s) land 7) <- %s' % tuple(cs))
            return e.code
        if r < 0.5:
            k = self.gen(INT, env, d - 1)
            a = self.gen(STRING, env, d - 1)
            e = self.combine([k, a], lambda cs: 'Hashtbl.replace gh ((%s) land 15) %s' % tuple(cs))
            return e.code
        if r < 0.55:
            a = self.gen(STRING, env, d - 1)
            return 'if Buffer.length gb < 200 then Buffer.add_string gb %s' % a.code
        if r < 0.65 and d > 0:
            v = self.fresh('i')
            lo = self.r.randint(-2, 2)
            hi = lo + self.r.randint(0, 4)
            save = self.mult
            self.mult *= 5
            try:
                body = self.gen_stmt(env + [Var(v, INT)], d - 1)
            finally:
                self.mult = save
            dirn = 'to'
            if self.chance(0.3):
                lo, hi, dirn = hi, lo, 'downto'
            return 'for %s = %d %s %d do %s done' % (v, lo, dirn, hi, body)
        if r < 0.7 and d > 0:
            c = self.fresh('cnt')
            save = self.mult
            self.mult *= 4
            try:
                body = self.gen_stmt(env, d - 1)
            finally:
                self.mult = save
            return 'let %s = ref 0 in while !%s < %d do incr %s; %s done' % (
                c, c, self.r.randint(0, 4), c, body)
        if r < 0.75:
            c = self.gen(BOOL, env, d - 1)
            s = self.gen_stmt(env, d - 1)
            return 'if %s then (%s)' % (c.code, s)
        if r < 0.8:
            lt = self.rand_type(1, False)
            l = self.gen(('list', lt), env, d - 1)
            v = self.fresh('it')
            s = self.gen_stmt(env + [Var(v, lt)], d - 1)
            return 'List.iter (fun %s -> %s) %s' % (v, s, l.code)
        if r < 0.85:
            t = self.rand_type(1)
            a = self.gen(t, env, d - 1)
            return 'print_endline ("  . " ^ %s %s)' % (self.show(t), a.code)
        if r < 0.9:
            t = self.rand_type(1)
            a = self.gen(t, env, d - 1)
            return 'ignore (%s : %s)' % (a.code, self.tstr(t))
        refs = [v for v in env if isinstance(v, Var) and v.ty[0] == 'ref']
        if refs:
            v = self.pick(refs)
            a = self.gen(v.ty[1], env, d - 1)
            return '%s := %s' % (v.name, a.code)
        recs = [v for v in env if isinstance(v, Var) and v.ty[0] == 'rec' and
                any(m for _, _, m in self.records[v.ty[1]])]
        if recs:
            v = self.pick(recs)
            f, ft, _ = self.pick([x for x in self.records[v.ty[1]] if x[2]])
            a = self.gen(ft, env, d - 1)
            return '(%s : %s).%s <- %s' % (v.name, v.ty[1], f, a.code)
        return 'incr gr'

    # ---------- declarations ----------
    def decl_types(self, out):
        names = []
        nrec = self.r.randint(0, 3)
        nvar = self.r.randint(0, 3)
        field_pool = ['fa', 'fb', 'fc', 'fd', 'fe', 'ff']
        ctor_pool = ['A', 'B', 'C', 'D', 'E', 'F', 'G']
        for i in range(nrec):
            self.records['r%d' % i] = None
        for i in range(nvar):
            self.variants['v%d' % i] = None
        for i in range(nrec):
            nf = self.r.randint(1, 4)
            fs = self.r.sample(field_pool, nf)
            self.records['r%d' % i] = [(f, None, self.chance(0.25)) for f in fs]
        for i in range(nvar):
            nc = self.r.randint(1, 4)
            cs = self.r.sample(ctor_pool, nc)
            self.variants['v%d' % i] = [(c, None) for c in cs]
        # fill in types now that names exist (recursive references allowed
        # only through list/option so values stay finite)
        for i in range(nrec):
            n = 'r%d' % i
            self.records[n] = [(f, self.field_type(n), m)
                               for f, _, m in self.records[n]]
        for i in range(nvar):
            n = 'v%d' % i
            cs = []
            for c, _ in self.variants[n]:
                k = self.r.random()
                if k < 0.35:
                    cs.append((c, []))
                else:
                    cs.append((c, [self.field_type(n) for _ in
                                   range(self.r.randint(1, 3))]))
            self.variants[n] = cs
        decls = []
        for n, fs in self.records.items():
            decls.append('%s = { %s }' % (n, '; '.join(
                '%s%s : %s' % ('mutable ' if m else '', f, self.tstr(ft))
                for f, ft, m in fs)))
            names.append(n)
        for n, cs in self.variants.items():
            decls.append('%s = %s' % (n, ' | '.join(
                c if not a else '%s of %s' % (c, ' * '.join(self.tstr(x) for x in a))
                for c, a in cs)))
            names.append(n)
        if decls:
            out.append('type ' + '\nand '.join(decls))
        if True:
            # polymorphic variants (their own declarations: a row may only
            # include an abbreviation that is already defined)
            if self.chance(0.6):
                small = [('X', INT), ('Y', None)]
                if self.chance(0.5):
                    small.append(('W', STRING))
                big = small + [('Z', ('tuple', (INT, BOOL))), ('Q', None)]
                self.pvs['pa'] = small
                self.pvs['pb'] = big
                self.pv_sub['pa'] = 'pb'
                out.append('type pa = [ %s ]' % ' | '.join(
                    '`%s' % t if p is None else '`%s of %s' % (t, self.tstr(p))
                    for t, p in small))
                out.append('type pb = [ pa | %s ]' % ' | '.join(
                    '`%s' % t if p is None else '`%s of %s' % (t, self.tstr(p))
                    for t, p in big[len(small):]))
            shows = []
            for n in self.records:
                fs = self.records[n]
                shows.append('show_%s (r : %s) = "{" ^ %s ^ "}"' % (
                    n, n, ' ^ ";" ^ '.join('"%s=" ^ %s r.%s' % (f, self.show(ft), f)
                                           for f, ft, _ in fs)))
            for n in self.variants:
                arms = []
                for c, a in self.variants[n]:
                    if not a:
                        arms.append('%s -> "%s"' % (c, c))
                    else:
                        vs = ['a%d' % j for j in range(len(a))]
                        arms.append('%s (%s) -> "%s(" ^ %s ^ ")"' % (
                            c, ', '.join(vs), c, ' ^ "," ^ '.join(
                                '%s %s' % (self.show(x), v) for x, v in zip(a, vs))))
                shows.append('show_%s (x : %s) = match x with %s' % (
                    n, n, ' | '.join(arms)))
            for n, tags in self.pvs.items():
                arms = []
                for tag, p in tags:
                    if p is None:
                        arms.append('`%s -> "`%s"' % (tag, tag))
                    else:
                        arms.append('`%s a -> "`%s(" ^ %s a ^ ")"' % (tag, tag, self.show(p)))
                shows.append('show_%s (x : %s) = match x with %s' % (n, n, ' | '.join(arms)))
            if shows:
                out.append('let rec ' + '\nand '.join(shows))

    def sanitize(self, t, allowed):
        """Wrap in option every declared type not in `allowed` that is not
        already under a list or option, so every type has finite values."""
        k = t[0]
        if k in ('rec', 'var'):
            return t if t[1] in allowed else ('option', t)
        if k in ('list', 'option'):
            return t
        if k in ('ref', 'array', 'lazy', 'box'):
            return (k, self.sanitize(t[1], allowed))
        if k == 'tuple':
            return ('tuple', tuple(self.sanitize(x, allowed) for x in t[1]))
        if k == 'arrow':
            return ('arrow', t[1], self.sanitize(t[2], allowed),
                    self.sanitize(t[3], allowed))
        return t

    def field_type(self, self_name):
        order = list(self.records) + list(self.variants)
        allowed = set(order[:order.index(self_name)])
        t = self.sanitize(self.rand_type(1, allow_arrow=self.chance(0.2)), allowed)
        if self.chance(0.15):
            return (self.pick(['list', 'option']), ('rec' if self_name.startswith('r') else 'var', self_name))
        return t

    def decl_exns(self, out):
        for i in range(self.r.randint(0, 3)):
            n = 'Ex%d' % i
            args = [] if self.chance(0.3) else [self.pick(BASE) for _ in range(self.r.randint(1, 2))]
            self.exns.append((n, args))
            out.append('exception %s%s' % (n, '' if not args else ' of ' + ' * '.join(self.tstr(a) for a in args)))

    def decl_modules(self, out):
        if not self.chance(0.6):
            return
        out.append('module type S = sig type t val make : int -> t val get : t -> int '
                   'val combine : t -> t -> t val show : t -> string end')
        impls = [
            'struct type t = int let make n = n * %d let get t = t '
            'let combine a b = a %s b let show = string_of_int end',
            'struct type t = string let make = string_of_int let get = String.length '
            'let combine a b = cap (a ^ b) let show s = s end',
            'struct type t = { mutable c : int; tag : string } let make n = { c = n; tag = "%s" } '
            'let get t = t.c + String.length t.tag let combine a b = { c = a.c - b.c; tag = b.tag } '
            'let show t = t.tag ^ string_of_int t.c end',
            'struct type t = int list let make n = [n; n + %d] let get = List.fold_left (+) 0 '
            'let combine = (@) let show l = show_list string_of_int l end',
        ]
        for i in range(self.r.randint(1, 3)):
            n = 'M%d' % i
            k = self.r.randint(0, len(impls) - 1)
            body = impls[k]
            if k == 0:
                body = body % (self.r.randint(1, 9), self.pick(['+', 'lxor', '*']))
            elif k == 2:
                body = body % self.pick(['p', 'q', 'zz'])
            elif k == 3:
                body = body % self.r.randint(0, 5)
            asc = ' : S' if self.chance(0.8) else ''
            out.append('module %s%s = %s' % (n, asc, body))
            self.mods.append(n)
        if self.chance(0.6):
            out.append('module F (X : S) : S = struct type t = X.t * int '
                       'let make n = (X.make n, n) let get (x, n) = X.get x + n '
                       'let combine (a, n) (b, m) = (X.combine a b, n - m) '
                       'let show (x, n) = X.show x ^ "/" ^ string_of_int n end')
            out.append('module FM = F (%s)' % self.pick(self.mods))
            self.mods.append('FM')
        if self.chance(0.3):
            out.append('module IM = struct include %s let extra = 1 end' % self.pick(self.mods))
        if self.chance(0.6):
            self.fcm = True
            out.append('let mods : (module S) list = [%s]' % '; '.join(
                '(module %s : S)' % m for m in self.mods))
            out.append('let use_mod (module X : S) n = X.show (X.combine (X.make n) (X.make (n + 1)))')
            self.env.append(Var('use_mod', arrow(('fcm',), arrow(INT, STRING))))

    def decl_gadt(self, out):
        if not self.chance(0.5):
            return
        p = self.pick(['G', 'K', 'Ex'])
        g = {'ty': 'gexp', 'int': p + 'Int', 'bool': p + 'Bool', 'str': p + 'Str',
             'add': p + 'Add', 'if': p + 'If', 'pair': p + 'Pair', 'fst': p + 'Fst',
             'eq': p + 'Eq', 'len': p + 'Len', 'eval': 'geval', 'size': 'gsize'}
        self.gadt = g
        out.append('''type _ gexp =
  | {int} : int -> int gexp
  | {bool} : bool -> bool gexp
  | {str} : string -> string gexp
  | {add} : int gexp * int gexp -> int gexp
  | {len} : string gexp -> int gexp
  | {eq} : int gexp * int gexp -> bool gexp
  | {if} : bool gexp * 'a gexp * 'a gexp -> 'a gexp
  | {pair} : 'a gexp * 'b gexp -> ('a * 'b) gexp
  | {fst} : ('a * 'b) gexp -> 'a gexp
let rec geval : type a. a gexp -> a = function
  | {int} n -> n
  | {bool} b -> b
  | {str} s -> s
  | {add} (a, b) -> geval a + geval b
  | {len} s -> String.length (geval s)
  | {eq} (a, b) -> geval a = geval b
  | {if} (c, a, b) -> if geval c then geval a else geval b
  | {pair} (a, b) -> (geval a, geval b)
  | {fst} p -> fst (geval p)
let rec gsize : type a. a gexp -> int = function
  | {int} _ | {bool} _ | {str} _ -> 1
  | {add} (a, b) -> 1 + gsize a + gsize b
  | {eq} (a, b) -> 1 + gsize a + gsize b
  | {len} s -> 1 + gsize s
  | {if} (c, a, b) -> 1 + gsize c + gsize a + gsize b
  | {pair} (a, b) -> 1 + gsize a + gsize b
  | {fst} p -> 1 + gsize p
let gint_only : int gexp -> int = function
  | {int} n -> n
  | {add} _ -> -1
  | {len} _ -> -2
  | {if} _ -> -3
  | {fst} _ -> -4'''.format(**g))
        self.env.append(Var('gint_only', arrow(('g', INT), INT)))

    def decl_classes(self, out):
        self.classes_sub = []
        if not self.chance(0.4):
            return
        out.append('class c0 (init : int) = object (self)\n'
                   '  val mutable st = init\n'
                   '  method get = st\n'
                   '  method bump k = st <- st + k; self#get\n'
                   '  method name = "c0"\nend')
        self.classes.append('c0')
        if self.chance(0.6):
            out.append('class c1 init = object\n'
                       '  inherit c0 init as super\n'
                       '  method! bump k = super#bump (%d * k)\n'
                       '  method! name = "c1"\n'
                       '  method extra = st * 2\nend' % self.r.randint(2, 5))
            self.classes_sub.append(('c1', 'c0'))

    # ---------- module system and objects (second round) ----------
    def decl_functors2(self, out):
        """Signature include, `with type` / `with module` constraints,
        destructive substitution, applicative functor paths."""
        r = self.r2
        if not self.mods or r.random() < 0.3:
            return
        m = r.choice(self.mods)
        k = r.randint(1, 9)
        out.append('module type S2 = sig include S val extra : t -> int end')
        out.append('module MkS2 (X : S) : S2 with type t = X.t = struct include X '
                   'let extra x = X.get x + %d end' % k)
        out.append('module P1 = MkS2 (%s)' % m)
        if r.random() < 0.7:
            # an applicative path: MkS2(m).t is P1.t
            out.append('type ap = MkS2(%s).t' % m)
            out.append('let fap (x : ap) : P1.t = x')
            out.append('let fp1 (n : int) : string = P1.show (fap (P1.make n)) ^ string_of_int '
                       '(P1.extra (P1.combine (P1.make n) (P1.make (n + %d))))' % r.randint(0, 5))
        else:
            out.append('let fp1 (n : int) : string = P1.show (P1.make n) ^ string_of_int '
                       '(P1.extra (P1.make n))')
        self.env.append(Var('fp1', arrow(INT, STRING), cost=4))
        if r.random() < 0.7:
            op = r.choice(['+', '-', 'lxor', 'land', 'lor'])
            out.append('module type SD = S with type t := int')
            out.append('module D1 : SD = struct let make n = n %s %d let get t = t '
                       'let combine a b = a %s b let show = string_of_int end'
                       % ('+', r.randint(0, 9), op))
            out.append('let fd1 (a : int) (b : int) : int = D1.get (D1.combine (D1.make a) (D1.make b))')
            self.env.append(Var('fd1', arrow(INT, arrow(INT, INT)), cost=3))
        if r.random() < 0.6:
            m2 = r.choice(self.mods)
            out.append('module type WM = sig module Inner : S val tag : string end')
            out.append('module type WM2 = WM with module Inner = %s' % m2)
            out.append('module W2 : WM2 = struct module Inner = %s let tag = "%s" end'
                       % (m2, r.choice(['w', 'wm', 'inner'])))
            out.append('module WithMod (Y : WM) = struct let g n = Y.tag ^ Y.Inner.show '
                       '(Y.Inner.make n) end')
            out.append('let fw (n : int) : string = let module R = WithMod (W2) in '
                       'R.g n ^ string_of_int (W2.Inner.get (%s.make n))' % m2)
            self.env.append(Var('fw', arrow(INT, STRING), cost=4))
        if r.random() < 0.4:
            out.append('module type SI = sig type t val v : t val show : t -> string end')
            out.append('module Twice (A : S) (B : S with type t = A.t) : SI with type t = A.t '
                       '= struct type t = A.t let v = B.combine (A.make %d) (A.make %d) '
                       'let show = A.show end' % (r.randint(0, 20), r.randint(0, 20)))
            out.append('module T2 = Twice (%s) (%s)' % (m, m))
            out.append('let ftw : string = T2.show T2.v')
            self.env.append(Var('ftw', STRING))

    def decl_recmods(self, out):
        r = self.r2
        if r.random() < 0.6:
            return
        if r.random() < 0.6:
            k1, k2, k3 = r.randint(1, 9), r.randint(1, 5), r.randint(0, 3)
            out.append(
                'module rec RA : sig\n'
                '  type t = Leaf of int | Node of RB.t list\n'
                '  val sum : t -> int\n  val mk : int -> t\nend = struct\n'
                '  type t = Leaf of int | Node of RB.t list\n'
                '  let sum = function Leaf n -> n\n'
                '    | Node l -> List.fold_left (fun a b -> a + RB.total b) 0 l\n'
                '  let mk n = if n <= %d then Leaf (n + %d) else Node [RB.mk (n - 1); RB.mk (n - 2)]\n'
                'end\n'
                'and RB : sig\n  type t = { w : int; kids : RA.t }\n'
                '  val total : t -> int\n  val mk : int -> t\nend = struct\n'
                '  type t = { w : int; kids : RA.t }\n'
                '  let total r = r.w * %d + RA.sum r.kids\n'
                '  let mk n = { w = n; kids = RA.mk (n - 1) }\nend' % (k3, k1, k2))
            out.append('let frec (n : int) : int = RA.sum (RA.mk (n land 7))')
            self.env.append(Var('frec', arrow(INT, INT), cost=40))
        else:
            k = r.randint(0, 15)
            out.append(
                'module rec Ev : sig val f : int -> bool val lz : int Lazy.t end = struct\n'
                '  let f n = n = 0 || Od.f (n - 1)\n'
                '  let lz = lazy (if Od.f %d then 1 else 2)\nend\n'
                'and Od : sig val f : int -> bool end = struct\n'
                '  let f n = n <> 0 && Ev.f (n - 1)\nend' % k)
            out.append('let fevn (n : int) : bool = Ev.f (n land 15) && Lazy.force Ev.lz > 0')
            self.env.append(Var('fevn', arrow(INT, BOOL), cost=20))

    def decl_objects2(self, out):
        r = self.r2
        if r.random() < 0.5:
            return
        k = r.randint(1, 4)
        out.append('class virtual oshape (k : int) = object (self)\n'
                   '  method virtual area : int\n'
                   '  method private scale x = x * k\n'
                   '  method describe = "shape:" ^ string_of_int (self#scale self#area)\n'
                   'end')
        out.append('class osq s = object\n  inherit oshape %d\n  val side = s\n'
                   '  method area = side * side\nend' % k)
        out.append('class osq2 s = object\n  inherit osq s as super\n'
                   '  method! area = super#area + %d\n'
                   '  method! describe = "sq2/" ^ super#describe\n'
                   '  method id : \'a. \'a -> \'a = fun x -> x\nend' % r.randint(0, 3))
        out.append('let odescribe (l : < describe : string; .. > list) =\n'
                   '  String.concat "," (List.map (fun o -> o#describe) l)')
        out.append('let fobj (n : int) : string = odescribe [ (new osq n :> oshape); '
                   '(new osq2 (n + 1) :> oshape) ] ^ string_of_int ((new osq2 n)#id n)')
        self.env.append(Var('fobj', arrow(INT, STRING), cost=6))
        if r.random() < 0.7:
            out.append('let oimm n = object\n  val mutable c = n\n'
                       '  method incr = c <- c + %d; c\n  method get = c\n'
                       '  method copy = {< c = c * 2 >}\nend' % r.randint(1, 3))
            out.append('let oget (o : < get : int; .. >) = o#get')
            out.append('let fimm (n : int) : int = let o = oimm n in let a = o#incr in '
                       'let b = (o#copy)#incr in a + b + oget o')
            self.env.append(Var('fimm', arrow(INT, INT), cost=6))
        if r.random() < 0.4:
            out.append('class type ocounter = object method get : int method next : ocounter end')
            out.append('class ocnt (n : int) : ocounter = object (_ : #ocounter)\n'
                       '  method get = n\n  method next = new ocnt (n + 1)\nend')
            out.append('let fcnt (n : int) : int = ((new ocnt n)#next#next)#get')
            self.env.append(Var('fcnt', arrow(INT, INT), cost=4))

    def decl_fun(self, out, i):
        name = 'f%d' % i
        params = []
        np = self.r.randint(1, 3)
        has_opt = False
        for j in range(np):
            pt = self.rand_type(1, allow_arrow=self.chance(0.3))
            c = self.r.random()
            if c < 0.2:
                params.append(('~l%d' % j, pt))
            elif c < 0.3 and j < np - 1:
                params.append(('?o%d' % j, pt))
                has_opt = True
            else:
                params.append(('', pt))
        if params[-1][0].startswith('?'):
            params[-1] = ('', params[-1][1])
        rt = self.rand_type(2, allow_arrow=self.chance(0.15))
        env = list(self.env)
        heads = []
        for lbl, pt in params:
            v = self.fresh('a')
            env.append(Var(v, pt, ceff=True))
            if lbl.startswith('~'):
                heads.append('~%s:(%s : %s)' % (lbl[1:], v, self.tstr(pt)))
            elif lbl.startswith('?'):
                dflt = self.const(pt) or '0'
                heads.append('?%s:(%s : %s = %s)' % (lbl[1:], v, self.tstr(pt), dflt))
            else:
                heads.append('(%s : %s)' % (v, self.tstr(pt)))
        self.cost = 0
        self.mult = 1
        try:
            body = self.gen(rt, env, self.r.randint(2, 5))
        except Budget:
            body = E(self.const(rt) or '(assert false)')
        cost = self.cost + 1
        ann = ''
        if self.chance(0.15):
            ann = self.pick(['[@inline never]', '[@inline]', '[@inline always]'])
        out.append('let%s %s %s : %s =\n  %s' % (ann, name, ' '.join(heads),
                                                 self.tstr(rt), body.code))
        fty = rt
        for lbl, pt in reversed(params):
            fty = arrow(pt, fty, lbl)
        self.env.append(Var(name, fty, ceff=body.eff, cost=cost))

    def decl_rec_fun(self, out, i):
        """A recursive function over a list (terminates structurally)."""
        name = 'rf%d' % i
        et = self.rand_type(1, False)
        rt = self.rand_type(1, False)
        x, tl, acc = self.fresh('x'), self.fresh('tl'), self.fresh('acc')
        env = list(self.env) + [Var(x, et), Var(acc, rt)]
        self.cost = 0
        self.mult = 1
        try:
            step = self.gen(rt, env, 3)
        except Budget:
            step = E(acc)
        out.append('let rec %s (l : %s) (%s : %s) : %s = match l with\n'
                   '  | [] -> %s\n  | %s :: %s -> %s %s (%s)' % (
                       name, self.tstr(('list', et)), acc, self.tstr(rt),
                       self.tstr(rt), acc, x, tl, name, tl, step.code))
        self.env.append(Var(name, arrow(('list', et), arrow(rt, rt)),
                            ceff=step.eff, cost=(self.cost + 1) * 8))

    def decl_value(self, out, i):
        t = self.rand_type(2)
        name = 'v%d' % i
        self.cost = 0
        self.mult = 1
        try:
            e = self.gen(t, self.env, self.r.randint(2, 5))
        except Budget:
            e = E(self.const(t) or '(assert false)')
        dflt = self.const(t)
        if dflt is None:
            out.append('let %s : %s = %s' % (name, self.tstr(t), e.code))
        else:
            out.append('let %s : %s = try %s with ex -> print_endline ("exn " ^ '
                       'Printexc.to_string ex); %s' % (name, self.tstr(t), e.code, dflt))
        out.append('let () = print_endline ("%s=" ^ %s %s)' % (name, self.show(t), name))
        self.env.append(Var(name, t))

    def decl_stmt(self, out):
        self.cost = 0
        self.mult = 1
        try:
            s = self.gen_stmt(self.env, 3)
        except Budget:
            s = '()'
        out.append('let () = try %s with ex -> print_endline ("exn " ^ Printexc.to_string ex)' % s)

    def program(self):
        out = [PRELUDE]
        self.env = list(PRELUDE_POLYS)
        self.decl_types(out)
        self.decl_exns(out)
        self.decl_modules(out)
        self.decl_gadt(out)
        self.decl_classes(out)
        self.decl_functors2(out)
        self.decl_recmods(out)
        self.decl_objects2(out)
        if self.ill:
            self.ill_at = self.r.randint(1, 300)
        nitems = self.r.randint(6, 18)
        fi = vi = 0
        for _ in range(nitems):
            k = self.r.random()
            if k < 0.35:
                self.decl_fun(out, fi)
                fi += 1
            elif k < 0.42:
                self.decl_rec_fun(out, fi)
                fi += 1
            elif k < 0.85:
                self.decl_value(out, vi)
                vi += 1
            else:
                self.decl_stmt(out)
        # call every function once on generated arguments and print
        for v in list(self.env):
            if isinstance(v, Var) and v.name.startswith(('f', 'rf')) and v.ty[0] == 'arrow':
                ps = []
                ty = v.ty
                while ty[0] == 'arrow':
                    ps.append((ty[1], ty[2]))
                    ty = ty[3]
                self.cost = 0
                self.mult = 1
                try:
                    call = self.gen_call(v, ps, self.env, 2)
                except Budget:
                    continue
                out.append('let () = print_endline ("%s:" ^ (try %s %s with ex -> "exn " ^ '
                           'Printexc.to_string ex))' % (v.name, self.show(ty), call.code))
        out.append('let () = print_endline ("gr=" ^ string_of_int !gr ^ " gs=" ^ show_string !gs '
                   '^ " ga=" ^ show_array string_of_int ga ^ " gb=" ^ show_string (Buffer.contents gb))')
        out.append('let () = print_endline ("gh=" ^ show_list (fun (k, v) -> string_of_int k ^ ":" ^ v) '
                   '(List.sort compare (Hashtbl.fold (fun k v acc -> (k, v) :: acc) gh [])))')
        self.items = out  # toplevel items, for genmf.py's split into units
        return '\n'.join(out) + '\n'


def enclosed(code):
    """code is one parenthesized expression: its first `(` closes last."""
    if not (code.startswith('(') and code.endswith(')')):
        return False
    depth = 0
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == '"':  # skip string literals
            i += 1
            while i < n and code[i] != '"':
                i += 2 if code[i] == '\\' else 1
        elif c == "'" and i + 2 < n and (code[i + 2] == "'" or code[i + 1] == '\\'):
            j = code.index("'", i + 2 if code[i + 1] != '\\' else i + 3)
            i = j
        elif c == '{' and code.startswith('{|', i):
            i = code.index('|}', i) + 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0 and i != n - 1:
                return False
        elif c in (MARK_OPEN, MARK_MID, MARK_CLOSE):
            return False
        i += 1
    return depth == 0


def strip(s):
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == MARK_OPEN:
            j = s.index(MARK_MID, i)
            i = j + 1
        elif c == MARK_CLOSE:
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def main():
    seed = int(sys.argv[1])
    ill = '--ill' in sys.argv
    marked = '--marked' in sys.argv
    g = Gen(seed, ill)
    p = g.program()
    if marked:
        sys.stdout.write(p)
        sys.stdout.write('(*NODES %s*)\n' % ' '.join(
            '%d=%s' % (k, v.encode().hex()) for k, v in g.nodes.items()))
    else:
        sys.stdout.write(strip(p))


if __name__ == '__main__':
    main()
