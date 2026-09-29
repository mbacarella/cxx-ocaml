type r = { a : exn; b : exn }
let f x = match x with { a = (Not_found | _) } -> 1
