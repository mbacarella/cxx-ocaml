module Base = struct type k = int type 'a t = 'a list let a = [] end
module type Loc = sig type k type 'a t val a : 'a t end
module F (X : Map.OrderedType) = struct
  module S : Loc = Base
  let z = 0
end
