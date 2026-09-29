module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
let f (x : N.t) = x
