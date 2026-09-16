type t = A | B
let f x = match x with (Not_found | _), A -> 1 | _, B -> 2
