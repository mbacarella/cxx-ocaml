let ( let* ) o f = f o
let ( and* ) = 1
let bad = let* x = 1 and* y = 2 in x
