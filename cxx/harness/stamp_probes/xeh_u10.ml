type t = A | B
let f x = match x with A, (Not_found | _) -> 1 | _, _ -> 2
