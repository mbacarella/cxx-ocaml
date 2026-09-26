let f ~x ~y = x - y
let a = f ~y:1 ~x:3
let g ?(z = 0) x = x + z
let b = g 3
let c = g ~z:1 3
let h = f ~x:1
