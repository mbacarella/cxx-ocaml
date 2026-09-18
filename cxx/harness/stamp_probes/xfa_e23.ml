module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
module N2 = N
let x = N2.empty
