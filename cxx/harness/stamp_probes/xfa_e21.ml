module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
module M = struct include N end
let x = M.empty
