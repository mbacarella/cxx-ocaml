module M = struct type t = int let compare = compare end
module F (X : Set.OrderedType) = struct
  module S : Map.S = Map.Make (M)
  let z1 = 1
  let z2 = 2
  let z3 = 3
end
