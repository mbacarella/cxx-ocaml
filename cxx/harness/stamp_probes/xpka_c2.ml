type z = int
let g (type s) set = let module Set = (val set : Set.S with type elt = s) in ()
