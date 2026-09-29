module M = struct type t = int let compare = compare end
module A = Map.Make (M)
module F (X : Map.OrderedType) = struct
  module S : Map.S = A
  let z = 0
end
