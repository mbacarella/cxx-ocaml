module type S = sig
  module type T = Set.S with type elt = int
end
let x = 1
