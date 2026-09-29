let f1 ?x y = ignore x; ignore y
let h = List.map (f1 ?x:None) [1]
