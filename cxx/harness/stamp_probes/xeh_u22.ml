type r = { a : exn; b : exn }
let f x = match x with { b = Exit } -> 2 | { a = Not_found } -> 1
