type z = int
let f set = let module Sx = (val set : Set.S with type elt = int) in ()
