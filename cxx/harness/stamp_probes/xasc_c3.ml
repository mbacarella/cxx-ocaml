module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make(X)
end
