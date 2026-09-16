let f1 ?x y = ignore x; ignore y
let foo (l : (int -> unit) list) = ignore l
let () = foo [f1]
