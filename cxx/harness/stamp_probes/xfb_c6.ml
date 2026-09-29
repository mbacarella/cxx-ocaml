module F (X : Map.OrderedType) (Y : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
  let z = 0
end
