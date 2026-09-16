let foo g () = g 1; ()
let f1 ?x y = ignore x; ignore y
let h = foo f1
