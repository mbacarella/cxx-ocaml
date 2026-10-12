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

Closure (non-flambda) compiler only; flambda compiles it.  The unused
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
  (Sys.opaque_identity "" = "") ||
  List.exists (if a then (fun _ -> a) else if true then raise (E 0) else (fun _ -> false)) []
let () = print_endline (try string_of_bool (f false) with _ -> "exn")
```

```
$ ocamlopt -O3 s3.ml
>> Fatal error: [Project_var] from a value with wrong approximation:
(project_var a/47 from anon_fn[s3.ml:4,25--37]/45=p/254)
closure=p/254
approx of closure={ descr=bottom var=p/254 symbol=<no symbol> }
```

Default flambda (-O1) compiles it.  The anonymous function in the dead
branch keeps a projection of `a` from a closure whose approximation is
bottom, and `simplify_project_var` treats that as an invariant
violation instead of dead code.  The regression copy is
`cxx/harness/fuzz/regress/flambda_fatal_format.ml`.
