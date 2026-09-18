module type S = sig
  open Set.Make(Bool)
  module type T = Set.S with type elt = t
end
let x = 1
