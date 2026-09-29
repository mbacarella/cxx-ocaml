module type Loc = sig type k type 'a t end
module F (X : Map.OrderedType) = struct
  module S : Loc = struct type k = int type 'a t = 'a list end
  let z = 0
end
