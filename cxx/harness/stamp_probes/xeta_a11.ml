let f1 ?x y = ignore x; ignore y
let h = List.map (let open List in f1) [1]
