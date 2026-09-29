let f ~a ~b c = a + b + c
let x = f 1 ~b:2 ~a:3
let g = f ~b:1
let y = g ~a:2 3
