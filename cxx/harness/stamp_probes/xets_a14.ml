let f1 ?x y = ignore x; ignore y
exception E of (int -> unit)
let h = E f1
