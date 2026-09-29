type t = A | B
let f x = match x with A, Not_found -> 1 | A, Exit -> 2 | B, _ -> 3
