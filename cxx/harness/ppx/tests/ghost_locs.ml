type t = { a : int; b : string }
let f { a; b } = a + String.length b
let g = f { a = 1; b = "x" }
let err = 1 + "not an int"
