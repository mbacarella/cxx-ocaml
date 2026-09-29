module type Loc = Map.S
module F (X : Map.OrderedType) = struct
  module S : Loc = Map.Make (X)
  let z = 0
end
