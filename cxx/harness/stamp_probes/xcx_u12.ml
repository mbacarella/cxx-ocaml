type _ g = G : 'a g | H : 'a list g
type _ e = E : 'a e
type rr = { p : int; q : int }
type _ gr = GR : 'a gr | HR : rr gr
let f (type a) (x : a g) (y : a) = match x, y with H, [] -> 0 | _, _ -> 1
