module F (X : Map.OrderedType) = struct
  module G = struct
    module S : Map.S = Map.Make (X)
  end
  let z = 0
end
