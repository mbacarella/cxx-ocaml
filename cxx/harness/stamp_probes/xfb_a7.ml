module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
  module T : Map.S = Map.Make (X)
  let z = 0
end
