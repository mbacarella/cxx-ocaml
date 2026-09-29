type z = int
let g (type s) set = let module Sx = (val set : Set.S with type elt = s) in ()
let f (type s) cmp =
  let module S = Set.Make (struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
