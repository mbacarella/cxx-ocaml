let f1 ?x y = ignore x; ignore y
type r = { f : ?x:unit -> int -> unit }
let h = { f = f1 }
