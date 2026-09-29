module type Loc = sig type k type 'a t val a : 'a t end
module F (X : Map.OrderedType) = struct
  module S : Loc with type k = int =
    struct type k = int type 'a t = 'a list let a = [] end
  let z = 0
end
