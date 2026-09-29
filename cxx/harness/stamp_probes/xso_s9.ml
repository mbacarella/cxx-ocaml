module type S = sig
  open Set.Make(Bool)
  include Set.S with type elt = int
end
let x = 1
