module F (X : Map.OrderedType) = struct
  module S : Map.S with type key = X.t = Map.Make (X)
  let z = 0
end
