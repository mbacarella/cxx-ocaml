type t = A of int | B of string
let f = function A x | B x -> x
