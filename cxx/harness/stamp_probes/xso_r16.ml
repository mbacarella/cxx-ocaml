module F (X : Set.OrderedType) = struct type t = int end
module type S = sig
  open F(Bool)
  type u = t
end
let x = 1
