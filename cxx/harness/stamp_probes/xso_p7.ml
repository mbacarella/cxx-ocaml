module type S = functor (X : Set.OrderedType) -> sig
  open Set.Make(Bool)
  type u = t
end
let x = 1
