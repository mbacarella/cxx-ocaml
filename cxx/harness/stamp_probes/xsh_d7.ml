module M = struct type t = int let compare = compare end
module F (X : Set.OrderedType) = struct
  module S : Map.S with type key = int = Map.Make (M)
  let z = 0
end
