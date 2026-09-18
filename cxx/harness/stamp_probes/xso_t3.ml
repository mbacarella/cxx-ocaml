module type S = sig
  open Set.Make(Bool)
  module M : Set.S with type elt = t
end
let x = 1
