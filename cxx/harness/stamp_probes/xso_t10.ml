module type S = sig
  open Set.Make(Bool)
  type u = t
  include Set.S with type elt = t
end
let x = 1
