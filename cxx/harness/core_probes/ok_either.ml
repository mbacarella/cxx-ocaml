let l = Either.Left 3
let f = function Either.Left x -> x | Either.Right y -> String.length y
