let f1 ?x y = ignore x; ignore y
let h = (f1 : int -> unit :> int -> unit)
