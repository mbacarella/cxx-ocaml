type z = int
let f (type s) set = let module Sx = (val set : Set.S with type elt = s) in ()
