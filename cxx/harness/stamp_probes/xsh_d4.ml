module M = struct type t = int let compare = compare end
module F (X : Set.OrderedType) = struct
  module S : Set.S = Set.Make (M)
  let z = 0
end
