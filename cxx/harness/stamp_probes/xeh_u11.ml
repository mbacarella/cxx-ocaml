type t = A | B
let f x = match x with A, Not_found -> 1 | B, _ -> 2
