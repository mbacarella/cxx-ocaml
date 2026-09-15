module F (X : Map.OrderedType) = struct
  let z = 0
  module S : Map.S = Map.Make (X)
end
