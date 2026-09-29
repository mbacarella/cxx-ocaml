let f1 ?x ?y z = ignore x; ignore y; ignore z
let h = List.map f1 [1]
