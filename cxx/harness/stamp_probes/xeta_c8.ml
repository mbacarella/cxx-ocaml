let f1 ?x y = ignore x; ignore y
let h = let g = f1 in g 1
