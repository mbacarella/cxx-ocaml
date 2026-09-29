type r = { a : int; mutable b : string }
let v = { a = 1; b = "x" }
let () = v.b <- "y"
let g r = r.a
