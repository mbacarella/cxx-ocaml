module Mk (Y : Map.OrderedType) = Map.Make (Y)
module F (X : Map.OrderedType) = struct
  module S : Map.S = Mk (X)
  let z = 0
end
