type _ g = G : 'a g | H : 'a list g
type _ e = E : 'a e
type rr = { p : int; q : int }
type _ gr = GR : 'a gr | HR : rr gr
let f (type a) (x : a g * a g) = match x with H, _ -> 0 | _, H -> 1
