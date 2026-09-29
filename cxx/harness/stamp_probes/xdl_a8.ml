module F (X : Set.OrderedType) : sig val y : int end = struct
  module S = Set.Make (X)
  let y = 0
end
