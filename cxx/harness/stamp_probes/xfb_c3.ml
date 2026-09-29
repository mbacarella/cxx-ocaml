module F (X : Map.OrderedType) = struct
  module S : sig type k type 'a t val a : 'a t end =
    struct type k = X.t type 'a t = 'a list let a = [] end
  let z = 0
end
