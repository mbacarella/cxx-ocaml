type t = A of exn * exn | B
let f x = match x with A (Not_found, Exit) -> 1 | B -> 2
