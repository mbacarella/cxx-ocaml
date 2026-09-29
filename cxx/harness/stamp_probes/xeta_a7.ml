let f1 ?x y = ignore x; ignore y
let h = List.map (if true then f1 else f1) [1]
