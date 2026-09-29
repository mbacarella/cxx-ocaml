type t = A of exn | B
let f x = match x with A (Not_found | _) -> 1 | B -> 2
