module F (X : Set.OrderedType) = Set.Make (X)
module N = F (Int)
module M : sig end = struct
  module N2 = F (Int) end
