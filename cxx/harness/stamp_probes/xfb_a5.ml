module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
  module type Q = sig type t val v : t end
  let z = 0
end
