type r = private {mutable a:int}
let f (x:r) = x.a <- 1
