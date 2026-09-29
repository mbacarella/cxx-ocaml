let f1 ?x y = ignore x; ignore y
let h : int -> unit :> int -> unit = f1
