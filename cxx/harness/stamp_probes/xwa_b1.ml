module F (X : Set.OrderedType) = struct
  module S : Set.S with type elt = X.t = Set.Make (X)
  let z = 0
end
