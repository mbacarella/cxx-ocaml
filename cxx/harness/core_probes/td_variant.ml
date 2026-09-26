type t = A | B of int | C of string * bool
let x = B 3
let f = function A -> 0 | B n -> n | C _ -> 1
