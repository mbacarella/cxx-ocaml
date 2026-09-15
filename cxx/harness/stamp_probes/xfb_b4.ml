module F (X : Set.OrderedType) = struct
  module S : Set.S = Set.Make (X)
  let z = 0
end
