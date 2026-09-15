module type Loc = sig type k type t val a : t end
module F (X : Map.OrderedType) = struct
  module S : Loc = struct type k = int type t = int let a = 0 end
  let z = 0
end
