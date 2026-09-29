module type S = sig
  module M : Set.S with type elt = int
end
let x = 1
