let f1 ?x y = ignore x; ignore y
type t = C of (int -> unit) * int
let h = C (f1, 1)
