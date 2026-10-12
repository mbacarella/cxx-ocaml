# Upstream OCaml bugs found by the port's differential fuzzer

Found 2026-10-11 by `cxx/harness/fuzz/` (random well-typed programs,
compiled by stock and by the port, compared at every level and run under
bytecode, closure, flambda and whole-program mode).  c++ocamlc /
c++ocamlopt reproduce each of them exactly, because stock is the
reference.  Not yet checked against the upstream issue tracker.

Reproduced on this tree's trunk (cfb30145d9, 5.6.0+dev); bug 1 also on
the 5.3.0 and 5.5.1 releases (the fuzzer saw it on 5.0 to 5.5.1).

## 1. `ocamlopt`: `Fatal error: Selection.size_expr` on a mutable let

```ocaml
let f b = Some (let x = ref 0 in b)
```

```
$ ocamlopt -c s1.ml
Warning 26 [unused-var]: unused variable x.
>> Fatal error: Selection.size_expr
Fatal error: exception Misc.Fatal_error
```

Native code, non-flambda `ocamlopt` only (opam 5.0.0, 5.1.1, 5.2.1,
5.3.0, 5.5.1 all fail, with or without -g/-O3/-inline 0).  Bytecode never
reaches instruction selection, so `ocamlc`, the `ocaml` toplevel, utop
and the js_of_ocaml playgrounds all accept it; flambda switches and
OxCaml compile it too.  The unused
`ref` becomes a `Clet_mut` around `b`, which ends up as an argument of
the `Some _` allocation; selecting that allocation calls `size_expr`
(`asmcomp/selectgen.ml`) on its arguments, and `size_expr` handles `Clet` and `Csequence` but
not `Clet_mut`; it falls into `| _ -> Misc.fatal_error
"Selection.size_expr"`.  About 3% of the fuzzer's generated programs hit
it.  Likely fix: a `Clet_mut(id, _, arg, body)` case mirroring `Clet`.

## 2. flambda: `compare` on two known chars folds to their difference

```ocaml
let () = Printf.printf "%d\n" (compare 'x' 'a')
```

| | bytecode | closure | flambda (any -O) |
|---|---|---|---|
| `compare 'x' 'a'` | 1 | 1 | **23** |
| same chars via `Sys.opaque_identity` | 1 | 1 | 1 |

`middle_end/flambda/simplify_primitives.ml`, the `[Value_char x;
Value_char y]` case:

```ocaml
| Pcompare_ints -> S.const_int_expr expr (Char.compare x y)
```

`Char.compare` is `code x - code y`; the runtime primitive this folds
(and the `Value_int` case just above, which uses `compare x y`) returns
-1/0/1.  Within the spec (only the sign of `compare` is specified), but
the result changes with optimization, and code that matches `compare`
against `-1 | 0 | 1` breaks under flambda only.  Fix: `compare x y`, as
in the int case.  All 38 stock bytecode-vs-flambda output disagreements
in a 5000-program run were this.

## 3. flambda -O2/-O3: `[Project_var] from a value with wrong approximation`

```ocaml
exception E of int
let f (a : bool) =
  List.exists (if a then (fun _ -> a) else if true then raise (E 0) else (fun _ -> false)) []
let () = ignore (f false)
```

What it needs (each checked by removing it): `f` must be used (no
`let ()` line: compiles); the raised value must be an exception
constructor applied to an argument right at the raise (`raise (E 0)`,
`raise (Failure "")`, `raise_notrace (E 0)` fail; `raise Exit`, `raise
Not_found`, a constant `exception E` raised, a prebuilt `let e = E 0 ..
raise e`, `failwith ""` and `assert false` compile); the dead `if true then raise
.. else ..` (raising directly in the `else`, or `if false`: compiles);
and a recursive higher-order callee such as `List.exists` or a local
`let rec exists` (a non-recursive `let app g = g 0`: compiles).  The
list may be empty or not.

```
$ ocamlopt -O3 -c s3.ml
>> Fatal error: [Project_var] from a value with wrong approximation:
(project_var a/38 from anon_fn[s3.ml:2,42--54]/36=p/198)
closure=p/198
approx of closure={ descr=bottom var=p/198 symbol=<no symbol> }

Fatal error: exception Misc.Fatal_error
```

Fails at -O2 and -O3 in every flambda release tested: 4.14.3, 5.0.0,
5.1.1, 5.2.1, 5.3.0, 5.4.1, 5.5.1 (each built from its tag with
--enable-flambda) and trunk.  Default flambda (-O1) compiles it.  The anonymous function in the dead
branch keeps a projection of `a` from a closure whose approximation is
bottom, and `simplify_project_var` treats that as an invariant
violation instead of dead code.  The regression copy is
`cxx/harness/fuzz/regress/flambda_fatal_format.ml`.
