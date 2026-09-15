module F (X : Map.OrderedType) : sig type u end = struct
  module S : Map.S = Map.Make (X)
  type u = int
end
