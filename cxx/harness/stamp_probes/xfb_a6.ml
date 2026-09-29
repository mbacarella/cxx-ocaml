module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
  exception E
  type q = int
end
