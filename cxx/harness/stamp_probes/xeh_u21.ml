type r = { a : exn; b : exn }
let f x = match x with { a = Not_found } -> 1 | { b = Exit } -> 2
