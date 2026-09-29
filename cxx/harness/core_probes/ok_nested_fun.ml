let f = fun x -> fun y -> fun z -> x + y + z
let g = f 1 2
let h x = let k y = x + y in k
