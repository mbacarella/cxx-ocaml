let f ~a ~b = a - b
let g = f ~b:3
let h ?(o = 0) ~x () = o + x
let k = h ~x:1
