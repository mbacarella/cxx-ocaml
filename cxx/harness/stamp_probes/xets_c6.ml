let f1 ?x y = ignore x; ignore y
let h = (f1 : ?x:unit -> int -> unit)
