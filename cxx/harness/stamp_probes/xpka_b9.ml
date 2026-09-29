type z = int
module S = Set.Make (Int)
let f () = (module S : Set.S with type elt = int)
let g () = (module S : Set.S with type elt = int)
