type _ g = G : 'a g | H : 'a list g
let f : type a. a g -> int -> a g -> int = fun x n y -> match x, y with
  H, H -> 0
  | G, _ -> 1
