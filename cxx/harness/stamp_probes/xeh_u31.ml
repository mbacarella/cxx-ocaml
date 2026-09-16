exception E of exn
let f x = match x with E (Not_found | _) -> 1
