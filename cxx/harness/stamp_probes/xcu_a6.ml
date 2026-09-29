module F (X : Set.OrderedType) = Set.Make (X)
module G = F (Int)
let g = G.empty
