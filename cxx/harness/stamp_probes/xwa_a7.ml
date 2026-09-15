module Mkl (Y : Map.OrderedType) =
  struct type k = Y.t type 'a t = 'a list let a = [] end
module type Loc = sig type k type 'a t val a : 'a t end
module F (X : Map.OrderedType) = struct
  module S : Loc = Mkl (X)
  let z = 0
end
