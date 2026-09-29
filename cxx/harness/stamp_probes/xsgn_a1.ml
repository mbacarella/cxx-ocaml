(* constructor / variant arguments and first parameters that are signed
   constants (simple_pattern's signed_constant): dune's
   `Unix.EUNKNOWNERR -1142` *)
type e = E of int | F of float
let f -1 = 2
let h = function `A -1 -> 0 | `A +2 -> 1 | _ -> 3
let k = function Some +1 -> 0 | Some - 3 -> 1 | _ -> 2
let m = function F -1.5 -> 0 | F +2. -> 1 | _ -> 2
let q = function (E -1142, x) -> x | (E - 1, _) -> 1 | _ -> 0
let a = function (Some -1 | None) -> 0 | _ -> 1
let w x = x -1
