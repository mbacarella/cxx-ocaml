type _ g = G : 'a g | H : 'a list g
let f (type a) = fun (x : a g) (y : a g) -> match x, y with H, H -> 0
  | G, _ -> 1
