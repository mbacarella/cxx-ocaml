(* FUZZ: levels=i,bc
   Found by fuzz.py (--ill seeds 602, 678, 681, 819, 973 and the probes after
   them).  `expr attribute` has LBRACKETAT's precedence: a trailing [@attr]
   belongs to the whole of `a + b`, `a :: b`, `-x`, `Some x`, `lazy x`,
   `a #+ b`, but to the right operand of `a = b`, `a @ b`, `a, b`, `a := b`
   (whose own location then ends at the attribute's `]`).  HASHOP is a
   simple_expr operator (`f a #+ b` is `f (a #+ b)`, `-1 #- 2` is
   `-(1 #- 2)`); a unary sign takes an application (`-1 2` is `-(1 2)`) or
   another signed expression (`- -1`), and folds an attribute-free constant
   operand (`-(1)`). *)
let ( #+ ) a b = a + b
let ( #- ) a b = a - b
let f x = x
let a = 1 and b = 2 and c = [3]
let l = [a + b [@x]; a * b [@x] + 1; - a [@x]; -1 [@x]; -(1) [@x]; f a #+ b; -1 #- 2; - -1; - - a;
         a #+ b [@x]; (lazy a [@x]) |> Lazy.force; (if a = b [@x] then 1 else 0); a #+ b #- a]
let m = (a :: [b] [@x] :: [c], a :: c [@x])
let p = (Some a [@x], `A b [@x], (a, b [@x]), (::) (a, []) [@x])
let r = ref 0
let () = r := a [@x]; r := !r + b [@x]
let () = assert (a < b) [@x]
let () = ignore (l, m, p, - f 2, -. 2. [@x], +1 [@x])
