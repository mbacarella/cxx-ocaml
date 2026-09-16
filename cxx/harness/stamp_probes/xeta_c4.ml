let f1 ?x y = ignore x; ignore y
let h = List.map (fun y -> f1 y) [1]
