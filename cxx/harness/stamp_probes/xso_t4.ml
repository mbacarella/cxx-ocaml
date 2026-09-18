module type S = sig
  open Set.Make(Bool)
  include Set.S with type elt = t list
end
let x = 1
