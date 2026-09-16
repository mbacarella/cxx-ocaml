let f1 ?x y = ignore x; ignore y
let h : [`A of int -> unit] = `A f1
