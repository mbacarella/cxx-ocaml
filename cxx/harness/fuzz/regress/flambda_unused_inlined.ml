(* FUZZ: levels=fl flags=fl:-O3
   Found by fuzz.py (seed 1225): at the end of the middle end flambda
   warns (55) about every application still carrying [@inlined] or
   [@unrolled] (flambda_middle_end.ml: "the optimizer did not know what
   function was being applied"); the port did not. *)
let f (g : float -> float) = (g [@inlined]) 1.
let h (g : int -> int) b = if b then (g [@inlined always]) 2 else (g [@inlined hint]) 3
let rec u (g : int -> int) n = if n = 0 then 0 else (g [@unrolled 2]) n + u g (n - 1)
let () = Printf.printf "%h %d %d\n" (f sqrt) (h succ true) (u pred 3)
