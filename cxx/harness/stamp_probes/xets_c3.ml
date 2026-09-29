let f1 ?x y = ignore x; ignore y
let h : int -> unit = fun y -> f1 y
