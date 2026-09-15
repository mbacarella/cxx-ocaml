module type Loc = sig type key type 'a t val empty : 'a t end
module F (X : Map.OrderedType) = struct
  module S : Loc with type key = X.t = Map.Make (X)
  let z = 0
end
