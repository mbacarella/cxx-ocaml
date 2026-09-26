let f ?(x = 1) () = x
let g h = h ()
let v = g f
