type _ g = G : 'a g | H : 'a list g
let f (type a) (x : a g) = match x with H -> (function (H : a g) -> 0 | G -> 1)
  | G -> fun _ -> 2
