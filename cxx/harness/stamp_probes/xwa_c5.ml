module type Loc = Map.S
module F (X : Map.OrderedType) = struct
  module S : Loc with type key = X.t = Map.Make (X)
  let z = 0
end
