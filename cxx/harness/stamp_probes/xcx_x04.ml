type _ g = G : 'a g | H : 'a list g
let f (type a) (x : a g) = match x with H -> (match x with H -> 0 | G -> 1)
  | G -> 2
