let f1 ~x y = ignore x; ignore y
let h = List.map (f1 ~x:1) [1]
