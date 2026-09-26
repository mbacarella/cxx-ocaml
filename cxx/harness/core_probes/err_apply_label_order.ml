let f ~a ~b = a + b
let g (h : a:int -> b:int -> int) = h
let bad = (g f) ~b:1 1
