let f1 ?x y = ignore x; ignore y
let h = (Some f1 : (int -> unit) option)
