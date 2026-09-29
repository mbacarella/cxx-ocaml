type z = int
let f (set : (module Set.S with type elt = int)) = let module Sx = (val set)
  in ()
