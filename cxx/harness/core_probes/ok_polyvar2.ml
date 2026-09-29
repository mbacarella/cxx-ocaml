let f = function `A | `B -> 1 | `C x -> x
let l = [`A; `C 3]
let g x = match x with #t -> 1
