type r = { f : int -> unit }
let f1 ?x y = ignore x; ignore y
let h = { f = f1 }
