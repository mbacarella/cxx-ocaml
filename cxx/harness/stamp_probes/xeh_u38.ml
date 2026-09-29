type t = A of exn * exn | B
let f x = match x with A ((Not_found | _), (Exit | _)) -> 1 | B -> 2
