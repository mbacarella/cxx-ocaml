let a = `A
let b = `B 3
let f = function `A -> 0 | `B n -> n
let c = f `A
let g x = match x with `C -> 1 | _ -> 0
let h (x : [< `X | `Y]) = match x with `X -> 1 | `Y -> 2
