type z = int
let g () = let module Set = struct end in ()
let f (type s) cmp =
  let module S = Set.Make (struct type t = s let compare = cmp end) in
  (module S : Set.S with type elt = s)
