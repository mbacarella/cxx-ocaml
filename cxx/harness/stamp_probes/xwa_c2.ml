module F (X : Map.OrderedType) = struct
  module S : sig type key type 'a t val empty : 'a t end = Map.Make (X)
  let z = 0
end
