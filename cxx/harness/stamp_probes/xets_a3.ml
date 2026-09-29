let f1 ?x y = ignore x; ignore y
type t = C of (int -> unit)
let h = C f1
