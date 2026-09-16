let f1 ?x y = ignore x; ignore y
let h : ?x:unit -> int -> unit = f1
