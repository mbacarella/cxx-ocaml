let f = function 0 -> "zero" | 1 | 2 -> "small" | n when n < 0 -> "neg" | _ -> "big"
let g x = match x with Some (a, b) -> a + b | None -> 0
