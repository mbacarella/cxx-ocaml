let f1 ?x y = ignore x; ignore y
let h : int -> unit = if true then f1 else f1
