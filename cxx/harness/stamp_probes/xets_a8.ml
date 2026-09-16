let f1 ?x y = ignore x; ignore y
let k () : int -> unit = f1
