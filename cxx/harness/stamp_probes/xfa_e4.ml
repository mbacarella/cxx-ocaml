module F (X : Set.OrderedType) = Set.Make (X)
let x = let module N = F (Int) in N.cardinal N.empty
