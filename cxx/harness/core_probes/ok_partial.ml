let add x y = x + y
let inc = add 1
let l = List.map (add 2) [1]
let f = List.fold_left
