module type S = sig
  include Set.S with type elt = int
end
let x = 1
