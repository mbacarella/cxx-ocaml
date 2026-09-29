let f1 ?x y = ignore x; ignore y
let h = List.map (ignore 1; f1) [1]
