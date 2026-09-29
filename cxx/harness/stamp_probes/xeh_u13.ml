type t = A | B
let f x = match x with Not_found, A -> 1 | _, B -> 2
