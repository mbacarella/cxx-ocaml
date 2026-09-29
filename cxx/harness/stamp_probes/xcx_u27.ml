type _ g = G : 'a g | H : 'a list g
type _ e = E : 'a e
type rr = { p : int; q : int }
type _ gr = GR : 'a gr | HR : rr gr
let f (type a b) (x : a g) (y : b g) = match x, y with H, H -> 0 | G, _ -> 1
  | _, G -> 2
