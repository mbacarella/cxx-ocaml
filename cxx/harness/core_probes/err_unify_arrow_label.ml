let f g = g ~x:1
let bad = f (fun ~y -> y)
