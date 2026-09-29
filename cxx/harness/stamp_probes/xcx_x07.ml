type _ g = G : 'a g | H : 'a list g
let f (type a) (x : a g) = match x with H -> (let H = x in 0) | G -> 2
