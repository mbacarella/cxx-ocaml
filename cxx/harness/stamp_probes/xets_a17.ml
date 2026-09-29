let f1 ?x y = ignore x; ignore y
type r = { f : int -> unit; g : int -> unit }
let h = { f = f1; g = f1 }
