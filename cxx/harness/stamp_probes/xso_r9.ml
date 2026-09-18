module type S = sig
  open Set.Make(Bool)
  include Set.S with type elt = t
end
let x = 1
