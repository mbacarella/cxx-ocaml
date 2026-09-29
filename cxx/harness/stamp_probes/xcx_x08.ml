type _ g = G : 'a g | H : 'a list g
let f (type a) (x : a g) = match x with H when (match x with H -> true
  | G -> false) -> 0 | _ -> 1
