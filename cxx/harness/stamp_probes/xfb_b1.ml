module F (X : Map.OrderedType) = struct
  module S = Map.Make (X)
  let z = 0
end
