let f1 ?x y = ignore x; ignore y
let h : type a. int -> unit = f1
