module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
  module T : Map.S with type key = X.t = Map.Make (X)
  let z = 0
end
