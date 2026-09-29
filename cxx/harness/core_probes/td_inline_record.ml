type t = A of { x : int; mutable y : string } | B
let f = function A r -> r.x | B -> 0
let g = A { x = 1; y = "" }
