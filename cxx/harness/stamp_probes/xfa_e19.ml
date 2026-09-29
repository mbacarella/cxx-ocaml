module F (X : Set.OrderedType) = Set.Make (X)
module M = F (Int)
let x = let module N = F (Int) in
  N.cardinal N.empty
